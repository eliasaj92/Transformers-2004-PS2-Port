// SPDX-License-Identifier: GPL-3.0-or-later
// Uses only an accelerated static package and private enhanced-build captures.
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_native_dual_math.h"
#include "tf_dual_state.h"
#include "tf_dual_memory.h"
#include <windows.h>
#include <array>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <xmmintrin.h>

bool ps2Vu1StaticPrepare(const uint8_t *, uint32_t, const PS2Memory *, uint64_t, uint32_t);
bool ps2Vu1StaticRun(int, void *, bool, uint8_t *, uint8_t *, GS &, PS2Memory *, uint32_t, uint32_t, uint32_t, uint32_t,
                     void *);
uint64_t ps2Vu1StaticFallbackCalls();
namespace
{
struct Dump
{
    std::array<uint32_t, 4> header;
    std::array<uint8_t, 16384> code, data;
    VU1State state;
    explicit Dump(const std::string &path)
    {
        static_assert(sizeof(state) == 664);
        std::ifstream file(path, std::ios::binary);
        file.read(reinterpret_cast<char *>(header.data()), sizeof(header));
        file.read(reinterpret_cast<char *>(code.data()), code.size());
        file.read(reinterpret_cast<char *>(data.data()), data.size());
        file.read(reinterpret_cast<char *>(&state), sizeof(state));
        if (!file || file.peek() != EOF || header[3] != sizeof(state))
            throw std::runtime_error("Bad capture");
    }
};
uint32_t word(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
unsigned emitted = 0;
} // namespace
void GS::processGIFPacket(const uint8_t *, uint32_t)
{
    throw std::runtime_error("Unframed packet");
}
void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool)
{
    ++emitted;
}
int main(int argc, char **argv)
{
    try
    {
        if (argc != 2 && argc != 3)
            throw std::runtime_error(
                "usage: native_dual_tail <private-selected-capture-list> [private-broad-capture-list]");
        _putenv_s("PS2X_VU1_FAST", "1");
        _putenv_s("PS2X_VU1_RUN_TO_END", "0");
        std::ifstream list(argv[1]);
        if (!list)
            throw std::runtime_error("Cannot open fixture list");
        std::array<uint8_t, 16384> resident{};
        std::array<uint8_t, 80000> transfer{};
        PS2Memory memory;
        memory.vu1Code = resident.data();
        GS gs;
        std::string name;
        unsigned total = 0, failures = 0, vertices = 0, tailVertices = 0;
        tf_native::DualTail tail;
        while (std::getline(list, name))
        {
            if (!name.empty() && name.back() == '\r')
                name.pop_back();
            if (name.empty())
                continue;
            if (!name.ends_with("-pre.bin"))
                throw std::runtime_error("Expected a pre capture");
            Dump before(name), expected(name.substr(0, name.size() - 7) + "post.bin");
            if (resident != before.code)
            {
                resident = before.code;
                ++memory.gen1;
            }
            const unsigned entry = before.header[0] & 0x7fffffffu, itop = before.header[2];
            const unsigned context = word(before.data.data() + 84);
            const unsigned csr = _mm_getcsr();
            if (!tf_native::prepareUnclippedDualTail(before.data.data(), before.state, itop, context, entry, tail))
                throw std::runtime_error("Qualified tail declined");
            if (_mm_getcsr() != csr)
                throw std::runtime_error("Tail preparation changed caller FP environment");
            if (!ps2Vu1StaticPrepare(resident.data(), resident.size(), &memory, memory.gen1, entry))
                throw std::runtime_error("Static package declined");
            const auto fallback = ps2Vu1StaticFallbackCalls();
            emitted = 0;
            auto state = before.state;
            const bool pending = ps2Vu1StaticRun(0, &state, true, resident.data(), tail.scratch.data(), gs, &memory,
                                                 entry, before.header[1], tail.tailItop, 40000000, transfer.data());
            if (pending || ps2Vu1StaticFallbackCalls() != fallback || emitted != 1)
                throw std::runtime_error("Tail escaped the accelerated package");
            const unsigned afterStaticCsr = _mm_getcsr();
            auto data = before.data;
            tf_native::finishUnclippedDualTail(data.data(), before.state, state, tail);
            unsigned stateDifferences = 0, rawDifferences = 0;
            const auto *actualBytes = reinterpret_cast<const uint8_t *>(&state);
            const auto *expectedBytes = reinterpret_cast<const uint8_t *>(&expected.state);
            for (unsigned offset = 0; offset < 660; ++offset)
            {
                if (offset == 638 || offset == 639 || (offset >= 649 && offset <= 651))
                    continue;
                stateDifferences += actualBytes[offset] != expectedBytes[offset];
            }
            gsnative::DualPacket packet;
            packet.version = 2;
            packet.vertices = tail.vertices;
            std::memcpy(packet.constants.data(), before.state.vf[1], 64);
            std::memcpy(packet.constants.data() + 64, before.data.data() + context * 16, 64);
            std::memcpy(packet.constants.data() + 128, before.data.data() + itop * 16 + 8, 4);
            for (unsigned i = 0; i < packet.vertices; ++i)
            {
                const auto *source = before.data.data() + (itop + 1 + 3 * i) * 16;
                std::memcpy(packet.inputs.data() + i * 32, source + 32, 12);
                std::memcpy(packet.inputs.data() + i * 32 + 16, source, 12);
            }
            const auto source = std::make_shared<gsnative::DualPacket>(packet);
            for (unsigned scenario = 0; scenario < 4; ++scenario)
            {
                auto deferredData = data, wanted = expected.data;
                tf_native::DualDeferredMemory deferred;
                deferred.record(deferredData.data(), itop, tail.tailVertices, source);
                if (tail.tailVertices < tail.vertices)
                {
                    const unsigned qword = itop + 1;
                    if (scenario == 1)
                    {
                        // An equal-value full write is still an overwrite. A
                        // snapshot comparison cannot discover this case.
                        deferred.overwrite(qword, 1);
                        std::memcpy(wanted.data() + qword * 16, deferredData.data() + qword * 16, 16);
                    }
                    else if (scenario == 2)
                    {
                        deferred.read(deferredData.data(), qword, 1);
                        deferredData[qword * 16 + 5] = wanted[qword * 16 + 5] = 0x5a;
                    }
                    else if (scenario == 3)
                    {
                        // A masked UNPACK can replace XYZ while preserving W.
                        // Only XYZ has deferred ownership; leave W untouched.
                        deferred.overwrite(qword, 1);
                        std::memset(deferredData.data() + qword * 16, 0x71, 12);
                        std::memset(wanted.data() + qword * 16, 0x71, 12);
                    }
                }
                deferred.read(deferredData.data());
                if (deferred.dirtyQwords || deferredData != wanted)
                    throw std::runtime_error("Deferred read/overwrite ownership mismatch");
            }
            {
                gsnative::DualFloatScope floating;
                if (!floating.ready)
                    throw std::runtime_error("Cannot set fallback FP mode");
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned i = 0; i < packet.vertices; ++i)
                    {
                        const auto raw = gsnative::dualRawVertex(packet, i, field);
                        auto *p = data.data() + (itop + 1 + field * (3 * packet.vertices + 4) + 3 * i) * 16;
                        const auto *e = expected.data.data() + (p - data.data());
                        rawDifferences += std::memcmp(raw.stq.data(), e, 12) != 0;
                        rawDifferences += std::memcmp(raw.xyz.data(), e + 32, 12) != 0;
                        if (i + tail.tailVertices >= tail.vertices)
                        {
                            rawDifferences += std::memcmp(p, e, 12) != 0;
                            rawDifferences += std::memcmp(p + 32, e + 32, 12) != 0;
                        }
                    }
            }
            gsnative::materializeDualPacket(packet, data.data() + (itop - 3) * 16, (8 + 6 * packet.vertices) * 16);
            if (_mm_getcsr() != afterStaticCsr)
                throw std::runtime_error("Fallback changed caller FP environment");
            const bool failed = stateDifferences || rawDifferences || data != expected.data;
            if (failed && failures < 8)
                std::printf("FAIL fixture=%u count=%u state_bytes=%u geometry=%u memory=%d\n", total, tail.vertices,
                            stateDifferences, rawDifferences, data != expected.data);
            failures += failed;
            ++total;
            vertices += tail.vertices;
            tailVertices += tail.tailVertices;
        }
        std::printf("DUAL_TAIL fixtures=%u failures=%u full_vertices=%u cpu_tail_vertices=%u; full semantic state, "
                    "memory, fallback geometry, FP environment\n",
                    total, failures, vertices, tailVertices);
        if (argc == 3)
        {
            std::ifstream setups(argv[2]);
            if (!setups)
                throw std::runtime_error("Cannot open broad fixture list");
            unsigned setupCount = 0, contextCount = 0;
            while (std::getline(setups, name))
            {
                if (!name.empty() && name.back() == '\r')
                    name.pop_back();
                if (name.empty())
                    continue;
                Dump before(name);
                const uint32_t entry = before.header[0] & 0x7fffffffu;
                if (!tf_native::isDualSetup(before.code.data(), entry, (before.header[0] & 0x80000000u) != 0))
                    continue;
                Dump expected(name.substr(0, name.size() - 7) + "post.bin");
                auto changedCode = before.code;
                changedCode[entry] ^= 1;
                if (tf_native::isDualSetup(changedCode.data(), entry, false) ||
                    tf_native::isDualSetup(before.code.data(), 8, false) ||
                    tf_native::isDualSetup(before.code.data(), entry, true))
                    throw std::runtime_error("Setup admission accepted a changed program/entry");
                if (resident != before.code)
                {
                    resident = before.code;
                    ++memory.gen1;
                }
                if (!ps2Vu1StaticPrepare(resident.data(), resident.size(), &memory, memory.gen1, entry))
                    throw std::runtime_error("Static package declined setup");
                // Poison every unclaimed data word. The full accelerated call
                // must still produce its captured state and leave memory alone.
                std::array<uint8_t, 16384> poisoned;
                poisoned.fill(0xa5);
                if (entry)
                    std::memcpy(poisoned.data(), before.data.data(), 64);
                else
                    std::memcpy(poisoned.data() + 64, before.data.data() + 64, 4);
                const auto originalPoison = poisoned;
                auto state = before.state;
                const auto fallback = ps2Vu1StaticFallbackCalls();
                emitted = 0;
                const bool pending =
                    ps2Vu1StaticRun(0, &state, false, resident.data(), poisoned.data(), gs, &memory, entry,
                                    before.header[1], before.header[2], 40000000, transfer.data());
                if (pending || emitted || ps2Vu1StaticFallbackCalls() != fallback || poisoned != originalPoison ||
                    before.data != expected.data || resident != before.code)
                    throw std::runtime_error("Setup touched undeclared state/memory/packets");
                const auto *actualBytes = reinterpret_cast<const uint8_t *>(&state);
                const auto *expectedBytes = reinterpret_cast<const uint8_t *>(&expected.state);
                for (unsigned offset = 0; offset < 660; ++offset)
                {
                    if (offset == 638 || offset == 639 || (offset >= 649 && offset <= 651))
                        continue;
                    if (actualBytes[offset] != expectedBytes[offset])
                        throw std::runtime_error("Setup read outside its declared data footprint");
                }
                ++setupCount;
                contextCount += entry != 0;
            }
            if (!setupCount)
                throw std::runtime_error("No setup fixtures admitted");
            std::printf("DUAL_SETUP fixtures=%u context=%u failures=0; all data outside setup inputs poisoned, full "
                        "state exact, no "
                        "data/code/GIF writes\n",
                        setupCount, contextCount);
        }
        return !total || failures ? 1 : 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "DUAL_TAIL ERROR: %s\n", e.what());
        return 1;
    }
}
