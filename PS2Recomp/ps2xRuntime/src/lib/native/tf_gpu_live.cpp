// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "tf_dual_state.h"
#include "tf_dual_memory.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

bool ps2Vu1StaticRun(int, void *, bool, uint8_t *, uint8_t *, GS &, PS2Memory *, uint32_t, uint32_t, uint32_t, uint32_t,
                     void *);
uint64_t ps2Vu1StaticFallbackCalls();

namespace tf_gpu_live
{
namespace
{
uint32_t word(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t wide(const uint8_t *p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void put(uint8_t *p, uint32_t v)
{
    std::memcpy(p, &v, 4);
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
std::atomic<uint64_t> selected{0}, attached{0}, declined{0};
std::atomic<uint64_t> bypassed{0}, tailVertices{0}, skippedVertices{0};
// VU-memory owner only; reported after producer shutdown.
uint64_t setupCalls = 0, contextSetupCalls = 0, readVertices[4] = {};
thread_local bool dropping = false;
thread_local unsigned dropped = 0;
struct Memory
{
    const void *owner = nullptr;
    uint8_t *data = nullptr;
    std::unique_ptr<tf_native::DualDeferredMemory> deferred;
} memoryState; // Serialized by the existing VU-memory fence/worker protocol.
struct Pending
{
    std::shared_ptr<gsnative::DualPacket> packet;
    bool emitted = false;
};
thread_local Pending pending;
struct Identity
{
    const void *owner = nullptr;
    uint64_t generation = 0;
    uint32_t dual = 0, clip = 0;
    bool valid = false;
};
thread_local Identity identity;

bool positiveW(const gsnative::DualPacket &packet)
{
    double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (unsigned i = 0; i < packet.vertices; ++i)
        for (unsigned lane = 0; lane < 3; ++lane)
        {
            const double v = std::bit_cast<float>(word(packet.inputs.data() + i * 32 + lane * 4));
            lo[lane] = std::min(lo[lane], v);
            hi[lane] = std::max(hi[lane], v);
        }
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *matrix = packet.constants.data() + field * 64;
        double minimum = std::bit_cast<float>(word(matrix + 60)), magnitude = std::abs(minimum);
        for (unsigned lane = 0; lane < 3; ++lane)
        {
            const double scale = std::bit_cast<float>(word(matrix + lane * 16 + 12));
            const double a = scale * lo[lane], b = scale * hi[lane];
            minimum += std::min(a, b);
            magnitude += std::max(std::abs(a), std::abs(b));
        }
        // Margin covers the separate binary32 operations and denormal flushes.
        // No skipped vertex may introduce a divide-by-zero/invalid flag.
        if (!(magnitude < 1e30 && minimum > 1e-5 * (magnitude + 1)))
            return false;
    }
    return true;
}
} // namespace
void install(uint32_t crc)
{
    const char *p = std::getenv("PS2X_TF_GPU_DUAL");
    if (!p || !*p || !std::strcmp(p, "0"))
        return;
    if ((std::strcmp(p, "1") && std::strcmp(p, "2")) || crc != 0x74c982c0 || enabled)
        throw std::runtime_error("PS2X_TF_GPU_DUAL=1/2 requires the supported SLUS-20668 executable");
    enabled = true;
    bypassEnabled = !std::strcmp(p, "2");
    if (bypassEnabled)
    {
        const auto equal = [](const char *name, const char *value) {
            const char *p = std::getenv(name);
            return p && !std::strcmp(p, value);
        };
        if (!equal("PS2X_GS_BACKEND", "hwgs") || !equal("PS2X_HWGS_API", "d3d12") ||
            !equal("PS2X_VU1_AOT_STATIC", "1") || !equal("PS2X_VU1_AOT", "1"))
            throw std::runtime_error("GPU geometry bypass requires D3D12 HWGS and accelerated static VU");
    }
    std::fprintf(stderr, "[gpu-dual] ordered live renderer enabled; CPU geometry bypass=%u\n", unsigned(bypassEnabled));
}

void readMemory(const void *owner, uint8_t *data, unsigned first, unsigned count, ReadReason reason)
{
    if (bypassEnabled && memoryState.owner == owner && memoryState.data == data && memoryState.deferred)
    {
        const auto before = memoryState.deferred->materializedVertices;
        memoryState.deferred->read(data, first, count);
        readVertices[reason] += memoryState.deferred->materializedVertices - before;
    }
}
bool prepareSetup(uint32_t entry, bool continuing, const void *owner, const uint8_t *code, uint8_t *data)
{
    if (!bypassEnabled || !tf_native::isDualSetup(code, entry, continuing))
        return false;
    readMemory(owner, data, entry ? 0 : 4, entry ? 4 : 1, InputRead);
    ++setupCalls;
    contextSetupCalls += entry != 0;
    return true;
}
void unpackMemory(const void *owner, uint8_t *data, unsigned address, unsigned count, unsigned cl, unsigned wl,
                  bool overwritesXYZ)
{
    if (!bypassEnabled || memoryState.owner != owner || memoryState.data != data || !memoryState.deferred ||
        !memoryState.deferred->dirtyQwords)
        return;
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned qword = (address + (cl >= wl ? (i / wl) * cl + i % wl : i)) & 1023;
        if (overwritesXYZ)
            memoryState.deferred->overwrite(qword, 1);
        else
            readMemory(owner, data, qword, 1, UnpackRead);
    }
}
void resetMemory(const void *owner)
{
    if (memoryState.owner == owner)
        memoryState = {};
}
bool suppressPacket()
{
    if (!dropping)
        return false;
    ++dropped;
    return true;
}
bool begin(uint32_t pc, bool continuing, uint32_t itop, uint64_t generation, const void *owner, const uint8_t *code,
           const uint8_t *data, const VU1State &state)
{
    if (!continuing || (pc != 0x10 && pc != 0xf8) || word(data + 64) != 2 || itop < 3 || itop > 1000)
        return false;
    if (bypassEnabled)
    {
        auto *writable = const_cast<uint8_t *>(data);
        readMemory(owner, writable, 0, 32, InputRead);
        if (itop < 1024)
            readMemory(owner, writable, itop, 1, InputRead);
        const unsigned context = word(data + 84);
        if (context <= 1009)
            readMemory(owner, writable, context, 15, InputRead);
        if (itop >= 3 && itop < 1024)
        {
            const unsigned n = word(data + itop * 16) & 0x7fff;
            const unsigned padded = (n + 3) & ~3u;
            const unsigned end = itop + std::max(padded * 3 + 1, n * 3 + 5);
            if (n >= 3 && n <= 28 && end <= 1024)
                readMemory(owner, writable, itop - 3, end - (itop - 3), InputRead);
        }
        readMemory(owner, writable, 1023, 1, InputRead);
    }
    const uint32_t count = word(data + itop * 16) & 0x7fff, context = word(data + 84);
    if (count < 3 || count > 28 || itop + 6 * count + 5 > 1023 || context > 1009 || word(data + itop * 16 + 4) != 3 ||
        wide(data + 21 * 16) || wide(data + 21 * 16 + 8))
        return false;
    const uint32_t dual = word(data + 80), clip = word(data + 96);
    if (dual > (16384 - 1712) / 8 || clip > (16384 - 3888) / 8)
        return false;
    if (identity.owner != owner || identity.generation != generation || identity.dual != dual || identity.clip != clip)
    {
        identity = {owner, generation, dual, clip,
                    hash(code, 1104) == 0xf84e7059a7f93d26ull && hash(code + dual * 8, 1712) == 0xd5cdafb0be7f3d85ull &&
                        hash(code + clip * 8, 3888) == 0xeea480a125c824a3ull};
    }
    if (!identity.valid)
        return false;
    if (pending.packet)
        throw std::runtime_error("Nested GPU geometry qualification");
    auto p = std::make_shared<gsnative::DualPacket>();
    p->vertices = count;
    p->generation = generation;
    std::memcpy(p->constants.data(), state.vf[1], 64);
    std::memcpy(p->constants.data() + 64, data + context * 16, 64);
    std::memcpy(p->constants.data() + 128, data + itop * 16 + 8, 4);
    std::memcpy(p->constants.data() + 132, data + (itop + 3 * count + 4) * 16, 8);
    for (unsigned i = 0; i < 33; ++i)
        if (!std::isfinite(std::bit_cast<float>(word(p->constants.data() + 4 * i))))
        {
            ++declined;
            return false;
        }
    for (unsigned i = 0; i < count; ++i)
    {
        const auto *source = data + (itop + 1 + 3 * i) * 16;
        auto *input = p->inputs.data() + i * 32;
        std::memcpy(input, source + 32, 12);
        std::memcpy(input + 16, source, 12);
        uint32_t color = 0;
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            const uint32_t c = word(source + 16 + lane * 4);
            if (c > 255)
            {
                ++declined;
                return false;
            }
            color |= c << (lane * 8);
        }
        put(input + 28, color);
        for (unsigned lane = 0; lane < 3; ++lane)
            if (!std::isfinite(std::bit_cast<float>(word(input + lane * 4))) ||
                !std::isfinite(std::bit_cast<float>(word(input + 16 + lane * 4))))
            {
                ++declined;
                return false;
            }
    }
    p->ticket = ++selected;
    pending = {std::move(p), false};
    return true;
}
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes)
{
    if (!pending.packet || pending.emitted)
        return {};
    pending.emitted = true;
    const auto &p = pending.packet;
    const unsigned count = p->vertices, fieldBytes = (4 + 3 * count) * 16;
    if (path != 1 || bytes != 2 * fieldBytes)
    {
        ++declined;
        return {};
    }
    for (unsigned field = 0; field < 2; ++field)
    {
        const uint8_t *f = data + field * fieldBytes;
        const uint64_t state = wide(f), tag = wide(f + 48);
        if ((state & 32767) != 2 || (state >> 58 & 3) || (state >> 60) != 1 || wide(f + 8) != 14 ||
            (tag & 32767) != count || (tag >> 58 & 3) || (tag >> 60) != 3 || wide(f + 56) != 0x412 ||
            (tag >> 47 & 7) != 4 || !(tag & (1ull << 46)))
        {
            ++declined;
            return {};
        }
        const unsigned context = (tag >> 56) & 1;
        if (p->version == 2 && (wide(f + 24) != 0x40 + context || wide(f + 40) != 0x18 + context))
        {
            ++declined;
            return {};
        }
        uint32_t firstDepth = 0;
        bool haveDepth = false, varies = false;
        for (unsigned i = 0; i < count; ++i)
        {
            const uint8_t *v = f + 64 + i * 48;
            uint32_t rgba = 0;
            for (unsigned lane = 0; lane < 4; ++lane)
                rgba |= (word(v + 16 + lane * 4) & 255) << (lane * 8);
            if (rgba != word(p->inputs.data() + i * 32 + 28))
            {
                ++declined;
                return {};
            }
            const uint32_t mask = word(p->constants.data() + 132 + (i / 16) * 4);
            const bool skip = ((mask << (i % 16)) & 32768) != 0;
            if (((word(v + 44) >> 15) & 1) != skip || (i < 2 && !skip))
            {
                ++declined;
                return {};
            }
            if (p->version == 2 || i < 2 || skip)
                continue;
            for (unsigned corner = i - 2; corner <= i; ++corner)
            {
                const uint32_t z = (word(f + 64 + corner * 48 + 40) >> 4) & 0xffffff;
                if (!haveDepth)
                {
                    firstDepth = z;
                    haveDepth = true;
                }
                else
                    varies |= z != firstDepth;
            }
        }
        if (varies)
            p->varyingDepth |= 1u << field;
    }
    ++attached;
    return p;
}

bool tryTail(uint8_t *code, uint8_t *data, VU1State &state, GS &gs, PS2Memory *memory, uint32_t entry, uint32_t top,
             uint32_t itop, uint32_t budget, void *transfer)
{
    if (!bypassEnabled || !pending.packet || !memory->instantWorker() || itop < 32 || state.dBitEnabled ||
        state.tBitEnabled || (state.status & 0x3c0u) != 0x3c0u || !positiveW(*pending.packet))
        return false;
    const unsigned count = pending.packet->vertices;
    if (count <= 4 || count == 17 || count == 18)
        return false;
    static thread_local tf_native::DualTail tail;
    if (!tf_native::prepareUnclippedDualTail(data, state, itop, word(data + 84), entry, tail))
        return false;
    const auto before = state;
    auto after = state;
    const auto fallback = ps2Vu1StaticFallbackCalls();
    dropped = 0;
    dropping = true;
    bool transferred;
    try
    {
        transferred = ps2Vu1StaticRun(0, &after, true, code, tail.scratch.data(), gs, memory, entry, top, tail.tailItop,
                                      budget, transfer);
    }
    catch (...)
    {
        dropping = false;
        throw;
    }
    dropping = false;
    if (transferred || dropped != 1 || ps2Vu1StaticFallbackCalls() != fallback)
        throw std::runtime_error("GPU geometry CPU tail changed accelerated control flow");
    tf_native::finishUnclippedDualTail(tail.preserved.data(), before, after, tail);
    auto &p = *pending.packet;
    p.version = 2;
    p.varyingDepth = 0;
    const unsigned skipped = count - tail.tailVertices;
    const unsigned bytes = (8 + 6 * count) * 16;
    const auto *gif = tail.preserved.data() + (itop - 3) * 16;
    // A conservative witness from resolved vertices belonging to real
    // triangles. A flat/insufficient tail uses the normal materialized draw.
    for (unsigned field = 0; field < 2; ++field)
    {
        bool have = false, varies = false;
        unsigned first = 0;
        for (unsigned i = std::max(2u, skipped + 1); i < count; ++i)
        {
            if ((word(p.constants.data() + 132 + (i / 16) * 4) << (i % 16)) & 32768)
                continue;
            for (unsigned j = std::max(i - 2, skipped); j <= i; ++j)
            {
                const unsigned z = (word(gif + field * (4 + 3 * count) * 16 + 64 + j * 48 + 40) >> 4) & 0xffffff;
                if (!have)
                {
                    first = z;
                    have = true;
                }
                else
                    varies |= first != z;
            }
        }
        if (varies)
            p.varyingDepth |= 1u << field;
    }
    const auto companion = packet(1, gif, bytes);
    if (!companion)
    {
        p.version = 1;
        p.varyingDepth = 0;
        pending.emitted = false;
        return false;
    }
    if (!memoryState.deferred)
    {
        memoryState.owner = memory;
        memoryState.data = data;
        memoryState.deferred = std::make_unique<tf_native::DualDeferredMemory>();
    }
    if (memoryState.owner != memory || memoryState.data != data)
        throw std::runtime_error("GPU geometry VU-memory owner changed without reset");
    std::memcpy(data, tail.preserved.data(), 16384);
    memoryState.deferred->record(data, itop, tail.tailVertices, companion);
    state = after;
    ++bypassed;
    tailVertices += tail.tailVertices;
    skippedVertices += skipped;
    memory->submitGifPacketWithGeometry(GifPathId::Path1, gif, bytes, true, false, companion);
    return true;
}
void end(bool transferred, bool fellBack)
{
    if (!pending.packet)
        return;
    if (transferred || fellBack || !pending.emitted)
        throw std::runtime_error("Qualified GPU geometry call changed its accelerated control flow");
    pending = {};
}
void finish()
{
    if (enabled)
        std::fprintf(stderr,
                     "[gpu-dual] selected=%llu attached=%llu declined=%llu; VU bypass=%llu tail_vertices=%llu "
                     "skipped_vertices=%llu\n",
                     static_cast<unsigned long long>(selected.load()), static_cast<unsigned long long>(attached.load()),
                     static_cast<unsigned long long>(declined.load()), static_cast<unsigned long long>(bypassed.load()),
                     static_cast<unsigned long long>(tailVertices.load()),
                     static_cast<unsigned long long>(skippedVertices.load()));
    if (memoryState.deferred)
    {
        const auto &d = *memoryState.deferred;
        std::fprintf(stderr,
                     "[gpu-dual:memory] records=%llu overwritten_qwords=%llu materialized_vertices=%llu "
                     "materialized_qwords=%llu dirty_qwords=%u\n",
                     d.recorded, d.overwrittenQwords, d.materializedVertices, d.materializedQwords, d.dirtyQwords);
    }
    if (bypassEnabled)
        std::fprintf(stderr,
                     "[gpu-dual:reads] setup_calls=%llu context_setup_calls=%llu ee_vertices=%llu input_vertices=%llu "
                     "unpack_vertices=%llu "
                     "program_vertices=%llu\n",
                     setupCalls, contextSetupCalls, readVertices[EeRead], readVertices[InputRead],
                     readVertices[UnpackRead], readVertices[ProgramRead]);
}
} // namespace tf_gpu_live
