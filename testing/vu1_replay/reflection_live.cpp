// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the exact live hooks against enhanced captures. Materialization here is
// diagnostic only, on a packet copy; the live renderer consumes the GPU source.
#include "runtime/gs/gs_native_reflection_vu.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
namespace tf_gpu_reflection
{
bool replayMemoryMatches(const void *, const uint8_t *, const uint8_t *);
}

namespace
{
const void *owner = nullptr;
uint8_t *memory = nullptr;
bool selected = false;
uint64_t packets = 0;
uint64_t carried = 0, externalQwords = 0;
std::vector<uint8_t> restored;
std::vector<gsnative::DualPacket> sources;
size_t nextSource = 0;
void skipOrdinary()
{
    while (nextSource < sources.size())
    {
        const unsigned n = sources[nextSource].vertices;
        if (n > 4 && n != 17 && n != 18)
            break;
        ++nextSource;
    }
}
} // namespace
void reflectionLiveInitialize()
{
    const char *path = std::getenv("TF_REFLECTION_LIVE_SOURCES");
    if (!path)
        throw std::runtime_error("Use authenticated private reflection source packets");
    const auto bytes = std::filesystem::file_size(path);
    if (!bytes || bytes % sizeof(gsnative::DualPacket) || bytes > 64 * 1024 * 1024)
        throw std::runtime_error("Invalid reflection sources length");
    sources.resize(bytes / sizeof(gsnative::DualPacket));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(sources.data()), bytes);
    if (!file)
        throw std::runtime_error("Cannot read reflection sources");
    _putenv_s("PS2X_TF_GPU_REFLECTION", "1");
    tf_gpu_reflection::install(0x74c982c0, true);
    tf_gpu_reflection::poisonForTest = true;
}
void reflectionLiveBegin(uint32_t pc, bool continuing, unsigned itop, uint64_t generation, const void *host,
                         const uint8_t *code, uint8_t *data)
{
    owner = host;
    memory = data;
    selected = tf_gpu_reflection::beginCall(
        pc, continuing, itop, generation, owner, code, data,
        [](const void *o, uint8_t *d, unsigned first, unsigned count) {
            tf_gpu_reflection::readMemory(o, d, first, count);
        },
        [](const void *, uint8_t *, unsigned, unsigned) {});
    if (!selected)
        tf_gpu_reflection::readMemory(owner, memory);
}
void reflectionLivePacket(uint32_t path, const uint8_t *&data, uint32_t count, bool drain, bool direct)
{
    if (auto source = tf_gpu_reflection::packet(path, data, count))
    {
        skipOrdinary();
        if (nextSource == sources.size())
            throw std::runtime_error("Unexpected extra reflection source");
        const auto &expected = sources[nextSource++];
        if (source->kind != expected.kind || source->vertices != expected.vertices || source->version != 2 ||
            source->inputs != expected.inputs || source->constants != expected.constants)
            throw std::runtime_error("Live source differs from the authenticated GPU inputs");
        if (path != 1 || !drain || direct)
            throw std::runtime_error("Reflection live transfer routing changed");
        restored.assign(data, data + count);
        gsnative::materializeReflectionPacket(*source, restored.data(), count);
        data = restored.data();
        ++packets;
    }
}
void reflectionLiveResolve(bool pending, bool fallback, bool last)
{
    if (selected)
        tf_gpu_reflection::endCall(pending, fallback);
    if (last)
        tf_gpu_reflection::readMemory(owner, memory);
    else
        carried += tf_gpu_reflection::hasDeferred(owner, memory);
}
void reflectionLiveExternalWrites(uint8_t *data, const uint8_t *before, const uint8_t *previous)
{
    for (unsigned qword = 0; qword < 1024; ++qword)
    {
        bool changed[4];
        for (unsigned lane = 0; lane < 4; ++lane)
            changed[lane] = std::memcmp(before + qword * 16 + lane * 4, previous + qword * 16 + lane * 4, 4) != 0;
        if (!(changed[0] || changed[1] || changed[2] || changed[3]))
            continue;
        if (changed[0] && changed[1] && changed[2])
            tf_gpu_reflection::overwriteXYZ(owner, data, qword, 1);
        else
            tf_gpu_reflection::readMemory(owner, data, qword, 1);
        for (unsigned lane = 0; lane < 4; ++lane)
            if (changed[lane])
                std::memcpy(data + qword * 16 + lane * 4, before + qword * 16 + lane * 4, 4);
        ++externalQwords;
    }
}
bool reflectionLiveMatches(const uint8_t *data, const uint8_t *expected)
{
    return tf_gpu_reflection::replayMemoryMatches(owner, data, expected);
}
void reflectionLiveFinish()
{
    skipOrdinary();
    const auto s = tf_gpu_reflection::stats();
    tf_gpu_reflection::finish();
    if (!packets || !carried || !externalQwords || nextSource != sources.size() || s.selected != packets ||
        s.records != packets || s.dirtyQwords || s.declined)
        throw std::runtime_error("Reflection live replay did not exercise complete paired offload");
    std::printf("REFLECTION_LIVE packets=%llu lighting_skipped=%llu projection_skipped=%llu poison=1 dirty=0 PASS\n",
                packets, s.lightingSkipped, s.projectionSkipped);
    std::printf("REFLECTION_CARRY calls=%llu external_qwords=%llu overwritten=%llu preserved_w=%llu\n", carried,
                externalQwords, s.overwritten, s.preservedW);
}
