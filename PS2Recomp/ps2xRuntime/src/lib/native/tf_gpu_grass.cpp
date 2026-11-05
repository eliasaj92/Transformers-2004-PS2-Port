// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_native_grass_vu.h"
#include "tf_grass_memory.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace tf_gpu_grass
{
namespace
{
// The VU worker owns this state while a call is active. Output is resolved at
// every call boundary before EE/VIF access can resume; queued GIF owns copies.
struct State
{
    const void *owner = nullptr;
    const uint8_t *code = nullptr;
    uint8_t *data = nullptr;
    uint64_t generation = 0;
    uint64_t callSelected = 0;
    bool active = false;
    std::shared_ptr<gsnative::DualPacket> emitter;
    std::deque<gsnative::DualPacketPtr> packets;
    tf_native::GrassDeferredMemory memory;
    Stats counts;
} state;
struct Identity
{
    const void *owner = nullptr;
    const uint8_t *code = nullptr;
    uint64_t generation = 0;
    unsigned base = 0;
    bool valid = false;
} identity;
uint64_t hash(const uint8_t *data, unsigned bytes)
{
    uint64_t value = 14695981039346656037ull;
    for (unsigned i = 0; i < bytes; ++i)
    {
        value ^= data[i];
        value *= 1099511628211ull;
    }
    return value;
}
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
void install(uint32_t crc, bool acceleratedGpuBypass)
{
    const auto *option = std::getenv("PS2X_TF_GPU_GRASS");
    if (!option || !*option || !std::strcmp(option, "0"))
        return;
    require(!std::strcmp(option, "1") && acceleratedGpuBypass && crc == 0x74c982c0 && !enabled,
            "GPU grass requires supported SLUS-20668 and accelerated D3D12 GPU bypass");
    enabled = true;
    std::fprintf(stderr, "[gpu-grass] ordered grass emission bypass enabled\n");
}
bool active()
{
    return enabled && state.active;
}
void beginCall(const void *owner, const uint8_t *code, uint8_t *data, uint64_t generation)
{
    require(enabled && owner && code && data && !state.active && !state.emitter && state.packets.empty() &&
                !state.memory.dirtyQwords,
            "Nested or unresolved GPU grass call");
    state.owner = owner;
    state.code = code;
    state.data = data;
    state.generation = generation;
    state.callSelected = state.counts.selected;
    state.active = true;
}
bool beginEmitter(uint32_t pc, uint8_t *data, const Input &input, bool overlapping, bool prologue)
{
    require(active() && state.data == data && !state.emitter, "Invalid grass emitter ownership");
    const unsigned base = (pc - 2536u) & 16383u;
    if (identity.owner != state.owner || identity.code != state.code || identity.generation != state.generation ||
        identity.base != base)
        identity = {state.owner, state.code, state.generation, base,
                    base <= 16384 - 3872 && hash(state.code + base, 3872) == 0xbbe99809fbf7361eull};
    bool finite = true;
    for (unsigned offset = 0; offset < input.size(); offset += 4)
    {
        float value;
        std::memcpy(&value, input.data() + offset, 4);
        finite &= std::isfinite(value);
    }
    if (!identity.valid || !finite)
    {
        ++state.counts.declined;
        return false;
    }
    auto p = std::make_shared<gsnative::DualPacket>();
    p->kind = gsnative::GeometryKind::GrassSprites;
    p->version = 2;
    p->vertices = 8;
    p->ticket = ++state.counts.selected;
    p->generation = state.generation;
    std::memcpy(p->inputs.data(), input.data(), input.size());
    state.emitter = std::move(p);
    state.counts.overlapping += overlapping;
    state.counts.skippedPrologues += prologue;
    return true;
}
void completeEmitter(uint8_t *data, unsigned base)
{
    if (!state.emitter)
        return;
    require(active() && state.data == data && base <= 1024 - 76 && state.packets.size() < 8,
            "Grass output exceeded the qualified memory/packet range");
    state.memory.record(data, base, state.emitter);
    state.packets.push_back(std::move(state.emitter));
}
void ordinary()
{
    if (active())
    {
        require(!state.emitter, "Declining a partially shortened grass emitter");
        state.memory.read(state.data);
    }
}
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes)
{
    if (!active() || state.packets.empty())
        return {};
    require(data && path == 1 && bytes == 1216, "Deferred grass transfer framing changed");
    auto result = std::move(state.packets.front());
    state.packets.pop_front();
    ++state.counts.attached;
    return result;
}
void endCall(bool transferred, bool fellBack)
{
    require(active() && !state.emitter && state.packets.empty(), "Unfinished GPU grass emission/transfer");
    // Conservatively restore the last occupied output buffers before any other
    // VU program or EE/VIF code can read them. Overwritten buffers cost nothing.
    state.memory.read(state.data);
    state.active = false;
    require(state.counts.selected == state.callSelected || (!transferred && !fellBack),
            "GPU grass call changed accelerated control flow");
}
Stats stats()
{
    auto result = state.counts;
    result.recorded = state.memory.recorded;
    result.reusedRecords = state.memory.reusedRecords;
    result.overwrittenQwords = state.memory.overwrittenQwords;
    result.materializedSprites = state.memory.materializedSprites;
    result.materializedQwords = state.memory.materializedQwords;
    result.dirtyQwords = state.memory.dirtyQwords;
    return result;
}
void finish()
{
    if (!enabled)
        return;
    const auto s = stats();
    std::fprintf(stderr,
                 "[gpu-grass] selected=%llu attached=%llu declined=%llu overlapping=%llu "
                 "skipped_iterations=%llu skipped_prologues=%llu recorded=%llu overwritten_qwords=%llu materialized_sprites=%llu "
                 "materialized_qwords=%llu dirty=%u reused_records=%llu\n",
                 s.selected, s.attached, s.declined, s.overlapping, s.selected * 2, s.skippedPrologues, s.recorded, s.overwrittenQwords,
                 s.materializedSprites, s.materializedQwords, s.dirtyQwords, s.reusedRecords);
}
} // namespace tf_gpu_grass
