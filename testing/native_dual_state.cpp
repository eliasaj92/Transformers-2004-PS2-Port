// SPDX-License-Identifier: GPL-3.0-or-later
// Offline preservation check against private enhanced-build captures.
#include "runtime/ps2_vu1.h"
#include "tf_dual_state.h"
#include <array>
#include <bitset>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <xmmintrin.h>

namespace {
struct Dump {
    std::array<uint32_t, 4> header;
    std::array<uint8_t, 16384> code, data;
    VU1State state;
    explicit Dump(const std::string& path) {
        static_assert(sizeof(state) == 664);
        std::ifstream file(path, std::ios::binary);
        file.read(reinterpret_cast<char*>(header.data()), sizeof(header));
        file.read(reinterpret_cast<char*>(code.data()), code.size());
        file.read(reinterpret_cast<char*>(data.data()), data.size());
        file.read(reinterpret_cast<char*>(&state), sizeof(state));
        if (!file || file.peek() != EOF || header[3] != sizeof(state))
            throw std::runtime_error("Malformed private accelerated capture");
    }
};
uint32_t word(const std::array<uint8_t, 16384>& data, size_t offset) {
    uint32_t value;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: native_dual_state <private-qualified-fixture-list>");
        std::ifstream fixtures(argv[1]);
        if (!fixtures) throw std::runtime_error("Cannot open private fixture list");
        size_t total = 0, failures = 0;
        std::string path;
        while (std::getline(fixtures, path)) {
            if (!path.empty() && path.back() == '\r') path.pop_back();
            if (path.empty()) continue;
            if (!path.ends_with("-pre.bin")) throw std::runtime_error("Expected a -pre.bin capture");
            Dump before(path), expected(path.substr(0, path.size() - 7) + "post.bin");
            auto data = before.data;
            tf_native::DualPreservation effects;
            const unsigned csr = _mm_getcsr();
            const int rounding = std::fegetround();
            if (!tf_native::preserveUnclippedDualState(data.data(), before.state, before.header[2],
                    word(data, 84), before.header[0] & 0x7fffffffu, effects))
                throw std::runtime_error("Qualified fixture declined: " + path);
            if (_mm_getcsr() != csr || std::fegetround() != rounding)
                throw std::runtime_error("Caller floating environment changed");
            std::bitset<1024> geometry;
            for (unsigned i = 0; i < effects.geometryQwordCount; ++i) geometry.set(effects.geometryQwords[i]);
            unsigned differentWords = 0;
            for (unsigned qword = 0; qword < 1024; ++qword)
                for (unsigned lane = 0; lane < 4; ++lane)
                    if ((!geometry[qword] || lane == 3) &&
                        std::memcmp(data.data() + qword * 16 + lane * 4, expected.data.data() + qword * 16 + lane * 4, 4)) {
                        if (failures < 8 && differentWords < 4)
                            std::printf("  memory q%u.%u actual=%08x expected=%08x\n", qword, lane,
                                word(data, qword * 16 + lane * 4), word(expected.data, qword * 16 + lane * 4));
                        ++differentWords;
                    }
            const bool vf17 = !std::memcmp(effects.vf17.data(), expected.state.vf[17], 16);
            const bool vf18 = !std::memcmp(effects.vf18.data(), expected.state.vf[18], 16);
            const bool cycles = effects.guestCycles == expected.state.cycles - before.state.cycles;
            if (differentWords || !vf17 || !vf18 || !cycles) {
                if (failures < 8) std::printf("FAIL %s words=%u vf17=%d vf18=%d cycles=%d\n",
                    std::filesystem::path(path).filename().string().c_str(), differentWords, vf17, vf18, cycles);
                ++failures;
            }
            ++total;
        }
        if (!total) throw std::runtime_error("Empty fixture list");
        std::printf("DUAL_STATE fixtures=%zu failures=%zu; non-geometry memory, VF17/VF18, cycles, FP environment\n", total, failures);
        std::puts("Offline candidate preservation only; full state and runtime bypass remain unqualified.");
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DUAL_STATE ERROR: %s\n", error.what());
        return 1;
    }
}
