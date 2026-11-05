// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_native_reflection_vu.h"
#include "tf_reflection_memory.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace tf_gpu_reflection
{
namespace
{
enum class Phase
{
    Idle,
    Prepared,
    Lit,
    Projecting,
    Kick,
    Attached,
    Ordinary
};
struct State
{
    const void *owner = nullptr;
    uint8_t *data = nullptr;
    const uint8_t *code = nullptr;
    uint64_t generation = 0;
    unsigned base = 0, count = 0, light = 0, projection = 0, lightContext = 0, projectionContext = 0, skipped = 0;
    Phase phase = Phase::Idle;
    ResolveMemory resolve = nullptr;
    ResolveMemory overwriteForeign = nullptr;
    std::shared_ptr<gsnative::DualPacket> source;
    std::unique_ptr<tf_native::ReflectionDeferredMemory> memory;
    Stats counts;
} state;
struct Identity
{
    const void *owner = nullptr;
    const uint8_t *code = nullptr;
    uint64_t generation = 0;
    unsigned light = 0, projection = 0, clip = 0;
    bool valid = false;
} identity;
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
uint32_t word(const void *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t wide(const void *p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
uint64_t hash(const uint8_t *p, unsigned bytes)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
void resolve(unsigned first, unsigned count)
{
    state.resolve(state.owner, state.data, first, count);
}
bool lightingDomain(const gsnative::DualPacket &p)
{
    const auto value = [](const uint8_t *v) { return std::bit_cast<float>(word(v)); };
    const auto *light = p.inputs.data() + gsnative::ReflectionLightingOffset;
    // The deferred implementation models normalized FMAC arithmetic. Keep the
    // unnormalized EFU length calculation away from overflow and a vanishing
    // view vector. Other inputs retain ordinary accelerated VU execution.
    for (unsigned i = 0; i < 44; ++i)
        if (std::abs(value(light + i * 4)) > 1e6f)
            return false;
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned axis = 0; axis < 3; ++axis)
            if (std::abs(value(light + column * 16 + axis * 4)) > 32.0f)
                return false;
    for (unsigned i = 0; i < p.vertices; ++i)
    {
        float distance = 0;
        const auto *v = p.inputs.data() + i * 32;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const float position = value(v + axis * 4);
            if (std::abs(position) > 1e6f || std::abs(value(v + 16 + axis * 4)) > 32.0f)
                return false;
            distance = std::max(distance, std::abs(value(light + 64 + axis * 4) - position));
        }
        if (!(distance > 1e-6f))
            return false;
    }
    return true;
}
} // namespace
void install(uint32_t crc, bool acceleratedGpuBypass)
{
    const char *option = std::getenv("PS2X_TF_GPU_REFLECTION");
    if (!option || !*option || !std::strcmp(option, "0"))
        return;
#ifndef PS2X_VU1_GPU_REFLECTION
    throw std::runtime_error("Rebuild the accelerated package with PS2X_VU1_GPU_REFLECTION=ON");
#endif
    require(!std::strcmp(option, "1") && acceleratedGpuBypass && crc == 0x74c982c0 && !enabled,
            "GPU reflection requires supported SLUS-20668 and accelerated D3D12 GPU bypass");
    enabled = true;
    std::fprintf(stderr, "[gpu-reflection] paired lighting/projection bypass enabled\n");
}
bool beginCall(uint32_t pc, bool continuing, unsigned itop, uint64_t generation, const void *owner, const uint8_t *code,
               uint8_t *data, ResolveMemory resolver, ResolveMemory overwriteForeign)
{
    require(state.phase == Phase::Idle, "Nested GPU reflection call");
    if (!enabled || !owner || !code || !data || !resolver || !overwriteForeign || !continuing ||
        (pc != 0x10 && pc != 0xf8) || itop < 96 || itop > 1000 || word(data + 64) != 3)
        return false;
    if (state.owner != owner || state.data != data)
    {
        require(!state.memory || !state.memory->dirtyQwords, "Reflection VU owner changed with unresolved memory");
        state.owner = owner;
        state.data = data;
    }
    state.resolve = resolver;
    state.overwriteForeign = overwriteForeign;
    resolve(0, 32);
    resolve(itop, 1);
    const unsigned n = word(data + itop * 16) & 32767, base = itop + 1;
    const unsigned light = word(data + 80), projection = word(data + 96), clip = word(data + 112);
    const unsigned lc = word(data + 84), pcx = word(data + 100), cc = word(data + 116);
    if (n <= 4 || n == 17 || n == 18 || n > gsnative::ReflectionMaxVertices || base - 4 + 4 * (4 + 3 * n) > 1024 ||
        lc > 1013 || pcx > 1007 || cc > 1000 || lc + 11 >= itop - 3 || pcx + 17 >= itop - 3 || cc + 24 >= itop - 3 ||
        word(data + itop * 16 + 4) != 4 || wide(data + 21 * 16) || wide(data + 21 * 16 + 8) ||
        light > (16384 - 1248) / 8 || projection > (16384 - 1728) / 8 || clip > (16384 - 3888) / 8)
        return false;
    for (unsigned slot = 4; slot <= 7; ++slot)
        if (wide(data + slot * 16) != wide(data + slot * 16 + 8))
            return false;
    if (identity.owner != owner || identity.code != code || identity.generation != generation ||
        identity.light != light || identity.projection != projection || identity.clip != clip)
        identity = {owner,
                    code,
                    generation,
                    light,
                    projection,
                    clip,
                    hash(code, 1104) == 0xf84e7059a7f93d26ull &&
                        hash(code + light * 8, 1248) == 0xf72677cdf8f26fdcull &&
                        hash(code + projection * 8, 1728) == 0x6ca5df87990a9276ull &&
                        hash(code + clip * 8, 3888) == 0xeea480a125c824a3ull};
    if (!identity.valid)
        return false;
    resolve(lc, 11);
    resolve(pcx, 17);
    resolve(cc, 24);
    resolve(base - 4, 3 * n + 18);
    resolve(1023, 1);
    readW(owner, data, base - 4, 4 * (4 + 3 * n));
    state.base = base;
    state.count = n;
    state.light = light * 8;
    state.projection = projection * 8;
    state.lightContext = lc;
    state.projectionContext = pcx;
    state.code = code;
    state.generation = generation;
    state.skipped = 0;
    state.phase = Phase::Prepared;
    return true;
}
bool active()
{
    return enabled && state.phase != Phase::Idle && state.phase != Phase::Ordinary;
}
bool lightingInput(uint32_t pc, unsigned base, unsigned count, unsigned context, uint8_t *data)
{
    if (state.phase != Phase::Prepared || pc != state.light || base != state.base || count != state.count ||
        context != state.lightContext || data != state.data)
        return false;
    auto source = std::make_shared<gsnative::DualPacket>();
    source->kind = gsnative::GeometryKind::Reflection;
    source->version = 2;
    source->vertices = count;
    source->generation = state.generation;
    std::memcpy(source->inputs.data() + gsnative::ReflectionLightingOffset, data + context * 16, 176);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto *p = data + (base + i * 3) * 16;
        auto *d = source->inputs.data() + i * 32;
        std::memcpy(d, p + 32, 12);
        std::memcpy(d + 12, p, 4);
        std::memcpy(d + 16, p + 16, 12);
        std::memcpy(d + 28, p + 4, 4);
    }
    if (!gsnative::reflectionSourceValid(*source) || !lightingDomain(*source))
        return false;
    source->ticket = ++state.counts.selected;
    state.source = std::move(source);
    return true;
}
void lightingDone(uint8_t *data)
{
    require(state.phase == Phase::Prepared && state.source && data == state.data, "Unowned reflection lighting tail");
    state.phase = Phase::Lit;
    state.counts.lightingSkipped += state.count - 4;
    if (poisonForTest)
        std::memset(data + state.base * 16, 0xcd, (state.count - 4) * 48);
}
bool projectionInput(uint32_t pc, unsigned base, unsigned count, unsigned context, const float *firstMatrix,
                     uint8_t *data)
{
    if (state.phase != Phase::Lit || pc != state.projection || base != state.base || count != state.count ||
        context != state.projectionContext || data != state.data || word(data + 21 * 16))
        return false;
    auto &p = *state.source;
    std::memcpy(p.constants.data(), firstMatrix, 64);
    std::memcpy(p.constants.data() + 64, data + context * 16, 64);
    std::memcpy(p.constants.data() + 128, data + (context + 16) * 16 + 12, 4);
    std::memcpy(p.constants.data() + 132, data + (base + 3 * count + 3) * 16, 8);
    if (!gsnative::reflectionSourceValid(p))
        return false;
    state.phase = Phase::Projecting;
    return true;
}
bool projecting()
{
    return state.phase == Phase::Projecting;
}
void skippedProjection(unsigned count)
{
    require(projecting() && !state.skipped && count > 0 && count + 3 <= state.count,
            "Unexpected reflection projection tail size");
    state.skipped = count;
    state.counts.projectionSkipped += count;
}
void projectionDone(uint8_t *data)
{
    require(projecting() && state.skipped && state.source && data == state.data, "Unowned reflection output");
    state.overwriteForeign(state.owner, data, state.base - 4, 4 * (4 + 3 * state.count));
    if (!state.memory)
        state.memory = std::make_unique<tf_native::ReflectionDeferredMemory>();
    state.memory->record(data, state.base, state.count - state.skipped, state.source);
    state.phase = Phase::Kick;
}
void ordinary()
{
    require(state.phase == Phase::Prepared || state.phase == Phase::Lit,
            "Cannot decline shortened reflection projection");
    if (state.phase == Phase::Lit)
    {
        gsnative::DualFloatScope floating;
        require(floating.ready, "Cannot restore declined reflection lighting");
        for (unsigned i = 0; i < state.count - 4; ++i)
        {
            const auto v = gsnative::reflectionLitVertex(*state.source, i);
            std::memcpy(state.data + (state.base + i * 3) * 16, &v, 48);
        }
    }
    resolve(0, 1024);
    state.source.reset();
    state.phase = Phase::Ordinary;
    ++state.counts.declined;
}
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes)
{
    if (state.phase != Phase::Kick)
        return {};
    require(data && path == 1 && bytes == 4 * (4 + 3 * state.count) * 16, "Reflection transfer framing changed");
    for (unsigned section = 0; section < 4; ++section)
    {
        const auto *f = data + section * (4 + 3 * state.count) * 16;
        const uint64_t tag = wide(f + 48);
        require((wide(f) & 32767) == 2 && (wide(f) >> 58 & 3) == 0 && wide(f) >> 60 == 1 && wide(f + 8) == 14 &&
                    (tag & 32767) == state.count && (tag >> 58 & 3) == 0 && tag >> 60 == 3 && wide(f + 56) == 0x412 &&
                    (tag >> 47 & 7) == 4 && (tag & (1ull << 46)) && ((tag >> 56) & 1) == section / 2 &&
                    bool(tag & 32768) == (section == 3) && wide(f + 24) == 0x40 + section / 2 &&
                    wide(f + 40) == 0x18 + section / 2,
                "Deferred reflection GIF contract changed");
    }
    state.phase = Phase::Attached;
    ++state.counts.attached;
    return state.source;
}
void endCall(bool transferred, bool fellBack)
{
    require(state.phase == Phase::Ordinary || (state.phase == Phase::Attached && !transferred && !fellBack),
            "Incomplete or interrupted reflection GPU call");
    state.source.reset();
    state.phase = Phase::Idle;
}
void readMemory(const void *owner, uint8_t *data, unsigned first, unsigned count)
{
    if (enabled && state.owner == owner && state.data == data && state.memory)
        state.memory->read(data, first, count);
}
void readW(const void *owner, uint8_t *data, unsigned first, unsigned count)
{
    if (enabled && state.owner == owner && state.data == data && state.memory)
        state.memory->readW(data, first, count);
}
void overwriteXYZ(const void *owner, uint8_t *data, unsigned first, unsigned count)
{
    if (enabled && state.owner == owner && state.data == data && state.memory)
        state.memory->overwriteXYZ(data, first, count);
}
bool hasDeferred(const void *owner, const uint8_t *data)
{
    return enabled && state.owner == owner && state.data == data && state.memory && state.memory->dirtyQwords;
}
#ifdef TF_REPLAY_REFLECTION_LIVE
// Diagnostic read of a copy: keep the real ownership map dirty across calls.
bool replayMemoryMatches(const void *owner, const uint8_t *data, const uint8_t *expected)
{
    std::array<uint8_t, 16384> copy;
    std::memcpy(copy.data(), data, copy.size());
    if (hasDeferred(owner, data))
    {
        auto cloned = std::make_unique<tf_native::ReflectionDeferredMemory>(*state.memory);
        cloned->read(copy.data());
    }
    return !std::memcmp(copy.data(), expected, copy.size());
}
#endif
void resetMemory(const void *owner)
{
    if (state.owner != owner)
        return;
    require(state.phase == Phase::Idle, "Reflection owner reset during execution");
    state.memory.reset();
    identity = {};
    state.owner = nullptr;
    state.data = nullptr;
}
Stats stats()
{
    auto s = state.counts;
    if (state.memory)
    {
        s.records = state.memory->recorded;
        s.overwritten = state.memory->overwrittenQwords;
        s.preservedW = state.memory->preservedW;
        s.litVertices = state.memory->litVertices;
        s.projectedVertices = state.memory->projectedVertices;
        s.dirtyQwords = state.memory->dirtyQwords;
    }
    return s;
}
void finish()
{
    if (!enabled)
        return;
    const auto s = stats();
    std::fprintf(stderr,
                 "[gpu-reflection] selected=%llu attached=%llu declined=%llu lighting_skipped=%llu "
                 "projection_skipped=%llu records=%llu overwritten=%llu preserved_w=%llu cpu_lit=%llu "
                 "cpu_projected=%llu dirty=%u\n",
                 s.selected, s.attached, s.declined, s.lightingSkipped, s.projectionSkipped, s.records, s.overwritten,
                 s.preservedW, s.litVertices, s.projectedVertices, s.dirtyQwords);
}
} // namespace tf_gpu_reflection
