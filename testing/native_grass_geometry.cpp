// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone qualification driver. Inputs/outputs are private captured data.
#include "../PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_geometry.h"
#include "../PS2Recomp/ps2xRuntime/src/lib/native/tf_grass_packets.h"
#include <windows.h>
#include <xmmintrin.h>
#include <array>
#include <bit>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace tf_native;
static_assert(sizeof(GrassBatch) == 640);

int selfTest() {
    GrassContext context{};
    context.clipScale = {1, 1, 1, 1};
    for (auto& field : context.transform) for (unsigned lane = 0; lane < 4; ++lane) field[lane][lane] = 1;
    context.density = 100;
    for (auto& weights : context.scatter) weights = {0.25f, 0.25f, 0, 0.5f};
    const std::array<GrassVertex, 3> vertices = {{{{-0.1f, 0, 1, 1}, {127, 127, 127, 127}},
                                                 {{0.1f, 0, 1, 1}, {127, 127, 127, 127}},
                                                 {{0, 0.1f, 1, 1}, {127, 127, 127, 127}}}};
    GrassStrip strip{vertices, {0, 0}};
    std::vector<GrassBatch> output;
    GrassStats stats;
    for (int rounding : {FE_TONEAREST, FE_UPWARD}) {
        if (std::fesetround(rounding)) throw std::runtime_error("Cannot set self-test rounding");
        const auto before = _mm_getcsr();
        if (!buildGrassGeometry(context, strip, output, stats) || output.size() != 3 ||
            stats.triangles != 1 || stats.coarseCulled != 0 || _mm_getcsr() != before || std::fegetround() != rounding)
            throw std::runtime_error("Synthetic triangle or floating environment test failed");
        const auto valid = context;
        context.density = std::bit_cast<float>(0x7f800001u); // Signaling NaN, never execute the shader.
        if (buildGrassGeometry(context, strip, output, stats) || !output.empty() || _mm_getcsr() != before)
            throw std::runtime_error("Rejected input leaked floating environment");
        context = valid;
        strip.restartMasks[0] = 1u << 13;
        if (!buildGrassGeometry(context, strip, output, stats) || !output.empty() || stats.coarseCulled != 1)
            throw std::runtime_error("Triangle-strip restart mask test failed");
        strip.restartMasks = {0, 0};
    }
    std::puts("Native grass self-test: triangle, restart mask, rejected signaling NaN, and caller rounding/CSR preservation passed.");
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") return selfTest();
        if (argc != 4) throw std::runtime_error("usage: native_grass_geometry <private-fixture-list> <fresh-output-directory> <repetitions>");
        const int repetitions = std::stoi(argv[3]);
        if (repetitions < 1 || repetitions > 10000) throw std::runtime_error("Repetitions must be 1..10000");
        const fs::path outputDirectory(argv[2]);
        if (!fs::create_directory(outputDirectory)) throw std::runtime_error("Use a fresh private output directory");
        std::ifstream list(argv[1]);
        if (!list) throw std::runtime_error("Cannot open fixture list");
        std::ofstream timing(outputDirectory / "timing.csv");
        timing << "fixture,groups,triangles,culled,subdivisions,max_depth,repetitions,thread_cycles\n";
        std::string line;
        size_t fixtures = 0, groups = 0;
        std::vector<GrassBatch> output;
        while (std::getline(list, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::ifstream file(line, std::ios::binary);
            std::array<uint32_t, 4> header;
            std::array<uint8_t, 16384> code, data;
            file.read(reinterpret_cast<char*>(header.data()), sizeof(header));
            file.read(reinterpret_cast<char*>(code.data()), code.size());
            file.read(reinterpret_cast<char*>(data.data()), data.size());
            if (!file || header[3] != 664 || (header[0] != 0x80000010 && header[0] != 0x800000f8))
                throw std::runtime_error("Not an admitted manager continuation: " + line);
            auto word = [&](size_t qword, size_t lane) {
                if (qword >= 1024 || lane >= 4) throw std::runtime_error("VU data address out of range");
                uint32_t value; std::memcpy(&value, data.data() + qword*16 + lane*4, 4); return value;
            };
            auto vector = [&](size_t qword) {
                if (qword >= 1024) throw std::runtime_error("VU vector address out of range");
                GrassVector value; std::memcpy(value.data(), data.data() + qword*16, 16); return value;
            };
            if (word(4, 0) != 1 || word(5, 0) != 624 || word(5, 1) != 53)
                throw std::runtime_error("Not the qualified grass stage table");
            const size_t itop = header[2], count = word(itop, 0) & 0x7fff;
            if (word(itop, 1) != 3 || count < 2 || count > 28) throw std::runtime_error("Unsupported grass descriptor");
            GrassContext context;
            context.clipScale = vector(53); context.clipOffset = vector(54); context.billboardOffset = vector(55);
            for (unsigned field = 0; field < 2; ++field)
                for (unsigned column = 0; column < 4; ++column) context.transform[field][column] = vector(56 + field*4 + column);
            const auto density = vector(76);
            context.depthBias = density[0]; context.density = density[2]; context.distanceScale = density[3];
            for (unsigned sample = 0; sample < 32; ++sample) context.scatter[sample] = vector(77 + sample);
            std::vector<GrassVertex> vertices(count);
            for (size_t vertex = 0; vertex < count; ++vertex) {
                vertices[vertex].position = vector(itop + 3 + vertex*3);
                vertices[vertex].position[3] = 1.0f; // V3-32's padding lane is not a position component.
                for (size_t lane = 0; lane < 4; ++lane) vertices[vertex].color[lane] = word(itop + 2 + vertex*3, lane);
            }
            GrassStrip strip{vertices, {static_cast<uint16_t>(word(itop + 3*count + 4, 0)),
                                        static_cast<uint16_t>(word(itop + 3*count + 4, 1))}};
            GrassStats stats;
            const auto initialCsr = _mm_getcsr();
            if (!buildGrassGeometry(context, strip, output, stats)) throw std::runtime_error("Native geometry declined: " + line);
            if (_mm_getcsr() != initialCsr) throw std::runtime_error("Native geometry leaked floating environment");
            const auto expected = output;
            ULONG64 first = 0, last = 0;
            if (!QueryThreadCycleTime(GetCurrentThread(), &first)) throw std::runtime_error("Cannot read thread cycle counter");
            for (int iteration = 0; iteration < repetitions; ++iteration)
                if (!buildGrassGeometry(context, strip, output, stats)) throw std::runtime_error("Native geometry benchmark declined");
            if (!QueryThreadCycleTime(GetCurrentThread(), &last)) throw std::runtime_error("Cannot read thread cycle counter");
            if (_mm_getcsr() != initialCsr || output.size() != expected.size() ||
                (!output.empty() && std::memcmp(output.data(), expected.data(), output.size()*sizeof(GrassBatch))))
                throw std::runtime_error("Native geometry changed across repeated calls");
            const std::string name = fs::path(line).stem().string();
            std::ofstream result(outputDirectory / (name + "-native.bin"), std::ios::binary);
            const uint32_t resultHeader[] = {0x53415247, 1, static_cast<uint32_t>(output.size()), stats.triangles,
                                             stats.coarseCulled, stats.subdivisions, stats.maxDepth, sizeof(GrassBatch)};
            result.write(reinterpret_cast<const char*>(resultHeader), sizeof(resultHeader));
            result.write(reinterpret_cast<const char*>(output.data()), output.size()*sizeof(GrassBatch));
            if (!result) throw std::runtime_error("Cannot write native geometry fixture");
            GrassPacketTemplates templates;
            if (!prepareGrassPackets(data, static_cast<uint32_t>(itop), 53, templates))
                throw std::runtime_error("Native packet templates declined: " + line);
            std::ofstream packets(outputDirectory / (name + "-native-gif.bin"), std::ios::binary);
            auto writePacket = [&](const void* bytes, uint32_t size) {
                const uint32_t metadata[] = {1, size, 1, 0}; // PATH1, immediate drain, no DIRECTHL.
                packets.write(reinterpret_cast<const char*>(metadata), sizeof(metadata));
                packets.write(static_cast<const char*>(bytes), size);
            };
            if (output.empty()) writePacket(templates.empty.data(), 16);
            for (size_t index = 0; index < output.size(); ++index) {
                GrassPacket packet;
                writeGrassPacket(templates, output[index], index, packet);
                writePacket(packet.data(), sizeof(packet));
            }
            if (!packets) throw std::runtime_error("Cannot write native packet fixture");
            timing << name << ',' << output.size() << ',' << stats.triangles << ',' << stats.coarseCulled << ','
                   << stats.subdivisions << ',' << stats.maxDepth << ',' << repetitions << ',' << last-first << '\n';
            ++fixtures; groups += output.size();
        }
        if (!fixtures || !timing) throw std::runtime_error("Empty fixture list or failed timing output");
        std::printf("Native grass: %zu fixtures, %zu groups; floating environment restored. Compare captured geometry before qualification.\n", fixtures, groups);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
    return 0;
}
