// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_dual_state.h"
#include <bitset>
#include <map>

// Included after the replay's capture helpers. This diagnostic consumes real
// GPU results, never expected geometry. The Python driver authenticates inputs
// and capture hashes before launching it. This is not a live renderer API.
class DualReplay {
    std::vector<uint8_t> inputs, batches, gpu;
    std::map<fs::path, size_t> selected;
    std::bitset<65536> seen;
    size_t used = 0;

    static uint32_t u32(const uint8_t* p) {
        uint32_t value;
        std::memcpy(&value, p, 4);
        return value;
    }
    static void require(bool condition, const char* text) {
        if (!condition) throw std::runtime_error(text);
    }
public:
    DualReplay(const fs::path& listPath, const fs::path& inputPath,
               const fs::path& batchPath, const fs::path& gpuPath)
        : inputs(readBytes(inputPath)), batches(readBytes(batchPath)), gpu(readBytes(gpuPath)) {
        require(!inputs.empty() && inputs.size() % 32 == 0 && inputs.size() <= 32 * 1024 * 1024,
                "Invalid uploaded dual inputs");
        require(!batches.empty() && batches.size() % 144 == 0 && batches.size() <= 65536 * 144,
                "Invalid uploaded dual batches");
        require(gpu.size() == 48 + inputs.size() * 2 && u32(gpu.data()) == 0x44554750 &&
                u32(gpu.data() + 4) == 1 && u32(gpu.data() + 8) == inputs.size() / 16 &&
                u32(gpu.data() + 12) == 32, "Invalid diagnostic GPU capture");
        std::ifstream file(listPath);
        std::string path;
        size_t index = 0;
        while (std::getline(file, path)) {
            if (!path.empty() && path.back() == '\r') path.pop_back();
            if (path.empty()) continue;
            require(selected.emplace(fs::weakly_canonical(path), index++).second,
                    "Duplicate dual fixture in selection");
        }
        require(file.eof() && index == batches.size() / 144, "Dual selection does not match GPU batches");
    }

    bool replace(const fs::path& path, Dump& actual, PS2Memory& memory,
                 std::bitset<1024>& geometry) {
        const auto found = selected.find(fs::weakly_canonical(path));
        if (found == selected.end()) return false;
        const size_t batchIndex = found->second;
        require(!seen[batchIndex], "Dual fixture replayed twice");
        const uint8_t* batch = batches.data() + batchIndex * 144;
        const unsigned itop = actual.header[2], context = word(actual.data, 84);
        const unsigned count = word(actual.data, itop * 16) & 0x7fff;
        const unsigned first = u32(batch + 140);
        const unsigned next = batchIndex + 1 == batches.size() / 144 ? unsigned(inputs.size() / 32) :
                              u32(batch + 144 + 140);
        require(count >= 3 && count <= 28 && first <= inputs.size() / 32 && next >= first &&
                next - first == count && next <= inputs.size() / 32 && context <= 1009 &&
                itop >= 3 && itop + 6 * count + 5 <= 1023, "Invalid selected dual range");
        require((actual.header[0] == 0x80000010u || actual.header[0] == 0x800000f8u) &&
                word(actual.data, 64) == 2 && word(actual.data, itop * 16 + 4) == 3,
                "Selected dual call no longer has its qualified layout");
        require(!std::memcmp(actual.state.vf[1], batch, 64) &&
                !std::memcmp(actual.data.data() + context * 16, batch + 64, 64) &&
                word(actual.data, itop * 16 + 8) == u32(batch + 128) &&
                !std::memcmp(actual.data.data() + (itop + 3 * count + 4) * 16, batch + 132, 8),
                "Carried transform constants differ from the actual GPU upload");
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t* source = actual.data.data() + (itop + 1 + i * 3) * 16;
            const uint8_t* uploaded = inputs.data() + (first + i) * 32;
            uint32_t rgba = 0;
            for (unsigned lane = 0; lane < 4; ++lane) {
                const uint32_t color = u32(source + 16 + lane * 4);
                require(color <= 255, "Carried color is outside the qualified format");
                rgba |= color << (lane * 8);
            }
            require(!std::memcmp(source + 32, uploaded, 12) && !std::memcmp(source, uploaded + 16, 12) &&
                    u32(uploaded + 12) == batchIndex && rgba == u32(uploaded + 28),
                    "Carried source geometry differs from the actual GPU upload");
        }
        tf_native::DualPreservation effects;
        require(tf_native::preserveUnclippedDualState(actual.data.data(), actual.state, itop, context,
                    actual.header[0] & 0x7fffffffu, effects), "Dual preservation declined qualified fixture");
        for (unsigned i = 0; i < effects.geometryQwordCount; ++i) geometry.set(effects.geometryQwords[i]);
        std::memcpy(actual.state.vf[17], effects.vf17.data(), 16);
        std::memcpy(actual.state.vf[18], effects.vf18.data(), 16);
        actual.state.pc = 0xf8;
        actual.state.cycles += effects.guestCycles;
        actual.state.ebit = actual.state.haltAfterDelaySlot = false;
        actual.state.stoppedByD = actual.state.stoppedByT = actual.state.branchPending = false;
        actual.state.branchTarget = 0xc8;
        actual.state.branchDelay = 0;
        actual.state.top = actual.header[1];
        actual.state.itop = itop;

        // Assemble a diagnostic packet from CPU bookkeeping plus GPU geometry.
        // Guest geometry stays unresolved, so later calls exercise dependencies.
        const uint8_t* begin = actual.data.data() + (itop - 3) * 16;
        std::vector<uint8_t> packet(begin, begin + (8 + 6 * count) * 16);
        for (unsigned i = 0; i < count; ++i) {
            for (unsigned field = 0; field < 2; ++field) {
                const uint8_t* raw = gpu.data() + 48 + (first + i) * 64 + field * 32;
                uint8_t* target = packet.data() + (field * (4 + count * 3) + 4 + i * 3) * 16;
                require(!std::memcmp(raw + 28, target + 44, 4), "GPU restart flags differ from preserved flags");
                std::memcpy(target, raw, 12);
                std::memcpy(target + 32, raw + 16, 12);
            }
        }
        memory.submitGifPacket(GifPathId::Path1, packet.data(), static_cast<uint32_t>(packet.size()));
        seen.set(batchIndex);
        ++used;
        return true;
    }

    size_t replaced() const { return used; }
    void finish() const { require(used == selected.size(), "Replay did not visit every selected GPU batch"); }
};
