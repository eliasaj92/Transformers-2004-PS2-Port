// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live_grass.h"
#include "tf_gpu_merge_guards.h"
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_dual_frontend.h"
#include "runtime/gs/gs_native_grass_math.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include <stdexcept>

namespace tf_gpu
{
namespace
{
uint32_t word(const void *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}
uint64_t wide(const void *p)
{
    uint64_t value;
    std::memcpy(&value, p, 8);
    return value;
}
void put(void *p, uint32_t value)
{
    std::memcpy(p, &value, 4);
}
bool culled(const GrassRasterState &raster, unsigned field, const uint8_t *first, const uint8_t *last)
{
    const auto &offset = raster.offset[field];
    const int ax = int(word(first) & 65535) - offset[0], ay = int(word(first + 4) & 65535) - offset[1];
    const int bx = int(word(last) & 65535) - offset[0], by = int(word(last + 4) & 65535) - offset[1];
    GSVector4i box(std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by));
    box -= ((box & GSVector4i(15)) == GSVector4i(0)) & GSVector4i(0, 0, 1, 1);
    return box.rempty() ||
           !(box + GSVector4i(0, 0, 1, 1)).rintersects(GSVector4i::load<false>(raster.scissor[field].data()));
}
struct Draw
{
    DualGeometry &pool;
    const gsnative::DualPacket &geometry;
    const uint8_t *packet;
    GrassLiveStats &stats;
    bool forceFallback;
    unsigned field = 0;
    std::array<uint8_t, 320> inputs{};
    GrassRasterState raster{};
    DualGeometry::Batch batch;
    uint32_t firstSource = 0;
    bool grouped = false;
    // GIF handlers load aligned qwords, including CPU fallback packets.
    alignas(16) std::array<uint8_t, 1216> restored;
    bool hasRestored = false;
};
const uint8_t *fullPacket(Draw &draw)
{
    if (draw.geometry.version != 2)
        return draw.packet;
    if (!draw.hasRestored)
    {
        std::memcpy(draw.restored.data(), draw.packet, draw.restored.size());
        gsnative::materializeGrassPacket(draw.geometry, draw.restored.data(), uint32_t(draw.restored.size()));
        draw.hasRestored = true;
    }
    return draw.restored.data();
}
bool submit(void *context, GSHWDrawConfig &config)
{
    auto &draw = *static_cast<Draw *>(context);
    if (draw.forceFallback || config.nverts != 16 || !GSDevice12::GetInstance()->CanRenderHWFromGPU(config))
        return false;
    if (!draw.batch)
    {
        draw.batch = draw.pool.prepareGrass(draw.inputs.data(), 1, &draw.raster);
        if (!draw.batch)
        {
            ++draw.stats.outputDeclined;
            return false;
        }
    }
    if (!draw.pool.draw(draw.batch, config, draw.firstSource + draw.field * 8, 8))
        return false;
    ++draw.stats.gpuDraws;
    draw.stats.groupedDraws += draw.grouped;
    return true;
}
void vertex(GSVertex &v, const uint8_t *xyz, const uint8_t *stq, const uint8_t *rgba)
{
    v = {};
    std::memcpy(&v.ST, stq, 8);
    v.RGBAQ.Q = 1.0f; // Framing checks every STQ, including the initial latch.
    v.RGBAQ.R = uint8_t(word(rgba));
    v.RGBAQ.G = uint8_t(word(rgba + 4));
    v.RGBAQ.B = uint8_t(word(rgba + 8));
    v.RGBAQ.A = uint8_t(word(rgba + 12));
    v.XYZ.X = uint16_t(word(xyz));
    v.XYZ.Y = uint16_t(word(xyz + 4));
    v.XYZ.Z = (word(xyz + 8) >> 4) & 0xffffff;
    v.FOG = (word(xyz + 12) >> 4) & 255;
}
void materialize(void *context, GSVertex *vertices, uint32_t count)
{
    auto &draw = *static_cast<Draw *>(context);
    if (count != 16)
        throw std::runtime_error("Unexpected grass fallback vertex count");
    ++draw.stats.materializations;
    const uint8_t *stq = draw.inputs.data() + 256 + draw.field * 16;
    const uint8_t *color = draw.inputs.data() + 288 + draw.field * 16;
    const uint8_t *field = fullPacket(draw) + draw.field * 608;
    for (unsigned sprite = 0; sprite < 8; ++sprite)
    {
        const auto *v = field + (6 + sprite * 4) * 16;
        const bool skip = (word(v + 60) & 0x8000) || culled(draw.raster, draw.field, v, v + 48);
        vertex(vertices[sprite * 2], v, stq, color);
        stq = v + 16;
        color = v + 32;
        vertex(vertices[sprite * 2 + 1], skip ? v : v + 48, stq, color);
    }
}
bool varyingDepth(const Draw &draw, unsigned field)
{
    // Only the final accelerated pair is resolved. Witness two surviving
    // sprites with distinct flat depths; never reconstruct skipped vertices
    // merely to qualify a native draw.
    const uint8_t *a = draw.packet + field * 608 + 30 * 16, *b = a + 64;
    return !(word(a + 60) & 0x8000) && !(word(b + 60) & 0x8000) && !culled(draw.raster, field, a, a + 48) &&
           !culled(draw.raster, field, b, b + 48) &&
           ((word(a + 56) >> 4) & 0xffffff) != ((word(b + 56) >> 4) & 0xffffff);
}
bool cachedBounds(const std::array<uint8_t, 320> &inputs, std::array<DualDrawBounds, 2> &bounds, GrassLiveStats &stats)
{
    ++stats.boundsQueries;
    static const bool enabled = [] {
        const char *option = std::getenv("PS2X_TF_GPU_GRASS_BOUNDS_CACHE");
        return option && !std::strcmp(option, "1");
    }();
    if (!enabled)
        return projectGrassBounds(inputs.data(), bounds);
    // Bounds depend on the source constants and initial STQ validation. Flat
    // sprite colors come from the emitted endpoint; incoming RGBA is unused by
    // projectGrassBounds. Raster offsets/scissors are applied after projection.
    constexpr unsigned keyBytes = 288;
    struct Entry
    {
        std::array<uint8_t, keyBytes> key{};
        std::array<DualDrawBounds, 2> bounds{};
        uint32_t hash = 0;
        bool occupied = false, valid = false;
    };
    // Fixed memory, no allocation/eviction walk on the GS thread. AVX2 is
    // already required by this renderer. CRC only selects a candidate: complete
    // key comparison is mandatory, including cached rejection results.
    static std::array<Entry, 8192> cache;
    uint64_t hash = 0;
    for (unsigned offset = 0; offset < keyBytes; offset += 8)
        hash = _mm_crc32_u64(hash, wide(inputs.data() + offset));
    auto &entry = cache[hash % cache.size()];
    if (entry.occupied && entry.hash == hash && !std::memcmp(entry.key.data(), inputs.data(), keyBytes))
    {
        ++stats.boundsCacheHits;
        stats.boundsCacheRejectedHits += !entry.valid;
        if (entry.valid)
            bounds = entry.bounds;
        return entry.valid;
    }
    const bool valid = projectGrassBounds(inputs.data(), bounds);
    std::memcpy(entry.key.data(), inputs.data(), keyBytes);
    entry.bounds = bounds;
    entry.hash = uint32_t(hash);
    entry.valid = valid;
    entry.occupied = true;
    return valid;
}
bool prepare(const gsnative::DualPacket &geometry, const uint8_t *packet, const uint32_t *incoming,
             PreparedGrass &result, GrassLiveStats &stats)
{
    result.geometry = &geometry;
    result.packet = packet;
    std::memcpy(result.inputs.data(), geometry.inputs.data(), 256);
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *f = packet + field * 608;
        std::memcpy(result.inputs.data() + 256 + field * 16, f + 64, 16);
        GIFRegSCISSOR scissor{};
        GIFRegXYOFFSET offset{};
        std::memcpy(&scissor, f + 16, 8);
        std::memcpy(&offset, f + 32, 8);
        if (scissor.SCAX0 > scissor.SCAX1 || scissor.SCAY0 > scissor.SCAY1)
            return false;
        result.raster.scissor[field] = {int(scissor.SCAX0) * 16 - 8, int(scissor.SCAY0) * 16 - 8,
                                        int(scissor.SCAX1) * 16 + 8, int(scissor.SCAY1) * 16 + 8};
        result.raster.offset[field] = {int(offset.OFX), int(offset.OFY), 0, 1};
    }
    std::memcpy(result.inputs.data() + 288, incoming, 16);
    std::memcpy(result.inputs.data() + 304, packet + 36 * 16, 16);
    return cachedBounds(result.inputs, result.bounds, stats);
}

void restoreTail(const uint8_t *field)
{
    alignas(16) std::array<uint8_t, 144> tail;
    std::memcpy(tail.data(), field + 80, 16);
    put(tail.data(), (word(tail.data()) & ~32767u) | 2);
    std::memcpy(tail.data() + 16, field + 30 * 16, 128);
    for (unsigned sprite = 0; sprite < 2; ++sprite)
        for (unsigned endpoint : {0u, 48u})
        {
            auto *flags = tail.data() + 16 + sprite * 64 + endpoint + 12;
            put(flags, word(flags) | 0x8000);
        }
    kzgs::ExecuteGIFOnGSThread(1, tail.data(), uint32_t(tail.size()));
}

struct MergedDraw
{
    DualGeometry &pool;
    const GrassPacketView *views;
    const PreparedGrass *prepared;
    uint32_t count;
    GrassLiveStats &stats;
    bool forceFallback;
    DualGeometry::Batch batch;
    unsigned field = 0;
};
bool submitMerged(void *context, GSHWDrawConfig &config)
{
    auto &draw = *static_cast<MergedDraw *>(context);
    if (draw.forceFallback || config.nverts != draw.count * 16 ||
        !draw.pool.draw(draw.batch, config, draw.field * draw.count * 8, draw.count * 8))
        return false;
    ++draw.stats.gpuDraws;
    ++draw.stats.mergedDraws;
    return true;
}
void materializeMerged(void *context, GSVertex *vertices, uint32_t count)
{
    auto &merged = *static_cast<MergedDraw *>(context);
    if (count != merged.count * 16)
        throw std::runtime_error("Unexpected merged grass fallback vertex count");
    for (unsigned i = 0; i < merged.count; ++i)
    {
        const auto &view = merged.views[i];
        Draw draw{merged.pool, *view.geometry, view.packet, merged.stats, true};
        draw.inputs = merged.prepared[i].inputs;
        draw.raster = merged.prepared[i].raster;
        draw.field = merged.field;
        materialize(&draw, vertices + i * 16, 16);
    }
}
void ordinaryMergedField(MergedDraw &merged)
{
    for (unsigned i = 0; i < merged.count; ++i)
    {
        const auto &view = merged.views[i];
        Draw draw{merged.pool, *view.geometry, view.packet, merged.stats, true};
        // Restore the first vertex's original packed RGBA before replaying a
        // field out of packet order. Its STQ is established by the field prefix.
        alignas(16) uint64_t rgba[4] = {1ull | (1ull << 60), GIF_REG_A_D, 0, GIF_A_D_REG_RGBAQ};
        const auto *color = merged.prepared[i].inputs.data() + 288 + merged.field * 16;
        rgba[2] = uint64_t(0x3f800000) << 32;
        for (unsigned channel = 0; channel < 4; ++channel)
            rgba[2] |= uint64_t(word(color + channel * 4) & 255) << (channel * 8);
        kzgs::ExecuteGIFOnGSThread(1, reinterpret_cast<const uint8_t *>(rgba), sizeof(rgba));
        kzgs::ExecuteGIFOnGSThread(1, fullPacket(draw) + merged.field * 608, 608);
    }
}
} // namespace

bool probeGrassMergeRanges(uint64_t &checks)
{
    return probeMergeRanges(checks);
}

template <unsigned Capacity>
static bool drawGrassGroupImpl(DualGeometry &pool, const GrassPacketView *views, uint32_t count, GrassLiveStats &stats,
                               bool forceFallback)
{
    const auto decline = [&](uint64_t &reason) {
        ++stats.mergeDeclined;
        ++reason;
        return false;
    };
    if (!views || count < 2 || count > Capacity || !g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12 ||
        g_gs_renderer->m_path[0].nloop || GSRendererHW::GetInstance()->GetUpscaleMultiplier() <= 1.0f)
        return decline(stats.mergeStateDeclined);
    auto *renderer = GSRendererHW::GetInstance();
    if (!renderer->m_env.PRMODECONT.AC || renderer->m_env.SCANMSK.MSK)
        return decline(stats.mergeStateDeclined);
    for (unsigned i = 0; i < count; ++i)
        if (!views[i].geometry || !grassPacketFraming(*views[i].geometry, views[i].packet, views[i].bytes))
            return decline(stats.mergeStateDeclined);
    const auto *first = views[0].packet;
    GIFRegPRIM prim{};
    prim.U64 = (wide(first + 80) >> 47) & 0x7ff;
    if (!prim.TME || prim.IIP || prim.FST || prim.AA1 || ((wide(first + 688) >> 47) & 0x7ff) != prim.U64)
        return decline(stats.mergeStateDeclined);
    for (unsigned i = 1; i < count; ++i)
        for (unsigned field = 0; field < 2; ++field)
        {
            const auto *a = first + field * 608, *b = views[i].packet + field * 608;
            if (wide(a + 16) != wide(b + 16) || wide(a + 32) != wide(b + 32) ||
                ((wide(b + 80) >> 47) & 0x7ff) != prim.U64)
                return decline(stats.mergeStateDeclined);
        }
    GIFRegSCISSOR scissor[2]{};
    std::memcpy(&scissor[0], first + 16, 8);
    std::memcpy(&scissor[1], first + 624, 8);
    for (const auto &s : scissor)
        if (s.SCAX0 > s.SCAX1 || s.SCAY0 > s.SCAY1)
            return decline(stats.mergeStateDeclined);
    if (!(scissor[0].SCAX1 < scissor[1].SCAX0 || scissor[1].SCAX1 < scissor[0].SCAX0 ||
          scissor[0].SCAY1 < scissor[1].SCAY0 || scissor[1].SCAY1 < scissor[0].SCAY0))
        return decline(stats.mergeStateDeclined);
    const auto &context = renderer->m_env.CTXT[prim.CTXT];
    if (!noFieldTextureFeedback(context, scissor[0], scissor[1]))
        return decline(stats.mergeFeedbackDeclined);
    std::array<PreparedGrass, Capacity> prepared;
    std::array<uint8_t, Capacity * 320> inputs;
    std::array<GrassRasterState, Capacity> raster;
    std::array<DualDrawBounds, 2> bounds;
    bool depthWitness[2]{};
    const auto &rgba = renderer->GetNativeVertexLatch().RGBAQ;
    uint32_t incoming[] = {rgba.R, rgba.G, rgba.B, rgba.A};
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &view = views[i];
        if (!prepare(*view.geometry, view.packet, incoming, prepared[i], stats))
            return decline(stats.mergeBoundsDeclined);
        std::memcpy(inputs.data() + i * 320, prepared[i].inputs.data(), 320);
        raster[i] = prepared[i].raster;
        Draw witness{pool, *view.geometry, view.packet, stats, false};
        witness.raster = raster[i];
        for (unsigned field = 0; field < 2; ++field)
        {
            depthWitness[field] |= varyingDepth(witness, field);
            if (i)
                mergeDrawBounds(bounds[field], prepared[i].bounds[field]);
            else
                bounds[field] = prepared[i].bounds[field];
        }
        for (unsigned channel = 0; channel < 4; ++channel)
            incoming[channel] = word(view.packet + 74 * 16 + channel * 4) & 255;
    }
    GSVertexTrace::Vertex minimum[2]{}, maximum[2]{};
    for (unsigned field = 0; field < 2; ++field)
    {
        auto state = context;
        state.SCISSOR = scissor[field];
        std::memcpy(&state.XYOFFSET, first + field * 608 + 32, 8);
        state.UpdateScissor();
        if (!depthWitness[field] || !prepareGrassTrace(bounds[field], state, prim, minimum[field], maximum[field]))
            return decline(stats.mergeBoundsDeclined);
    }
    auto batch = pool.prepareGrass(inputs.data(), count, raster.data(), true);
    if (!batch)
        return decline(stats.outputDeclined);
    MergedDraw draw{pool, views, prepared.data(), count, stats, forceFallback, std::move(batch)};
    std::array<uint16_t, Capacity * 16> indices;
    for (unsigned i = 0; i < count * 16; ++i)
        indices[i] = uint16_t(i);
    ++stats.mergedGroups;
    stats.mergedPackets += count;
    stats.largestMerge = std::max(stats.largestMerge, uint64_t(count));
    stats.packets += count;
    // Within each field the original primitive order is unchanged. The other
    // field accesses disjoint pixels, and neither target is sampled as texture.
    // Needs proper testing on additional levels before enabling by default.
    for (unsigned field = 0; field < 2; ++field)
    {
        draw.field = field;
        kzgs::ExecuteGIFOnGSThread(1, first + field * 608, 80);
        if (renderer->DrawNativeSprites(minimum[field], maximum[field], indices.data(), count * 16, true, &draw,
                                        submitMerged, materializeMerged))
            restoreTail(views[count - 1].packet + field * 608);
        else
        {
            stats.declinedFields += count;
            ordinaryMergedField(draw);
        }
    }
    return true;
}

bool drawGrassGroup(DualGeometry &pool, const GrassPacketView *views, uint32_t count, GrassLiveStats &stats,
                    bool forceFallback)
{
    // Keep scratch initialization proportional to the attempted group. Most
    // rejected wide batches eventually reach the small existing fallback.
    if (count <= 8)
        return drawGrassGroupImpl<8>(pool, views, count, stats, forceFallback);
    if (count <= 16)
        return drawGrassGroupImpl<16>(pool, views, count, stats, forceFallback);
    if (count <= 32)
        return drawGrassGroupImpl<32>(pool, views, count, stats, forceFallback);
    return drawGrassGroupImpl<MaxMergedGrassPackets>(pool, views, count, stats, forceFallback);
}

bool grassPacketFraming(const gsnative::DualPacket &p, const uint8_t *packet, uint32_t bytes)
{
    if (!packet || p.kind != gsnative::GeometryKind::GrassSprites || (p.version != 1 && p.version != 2) ||
        p.vertices != 8 || bytes != 1216)
        return false;
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *f = packet + field * 608;
        const uint64_t state = wide(f), st = wide(f + 48), tag = wide(f + 80);
        const unsigned context = (tag >> 56) & 1;
        if ((state & 32767) != 2 || (state >> 58 & 3) || state >> 60 != 1 || wide(f + 8) != 14 ||
            wide(f + 24) != 0x40 + context || wide(f + 40) != 0x18 + context || (st & 32767) != 1 || (st >> 58 & 3) ||
            st >> 60 != 1 || wide(f + 56) != 2 || (tag & 32767) != 8 || (tag >> 58 & 3) || tag >> 60 != 4 ||
            wide(f + 88) != 0x4124 || !(tag & (1ull << 46)) || !(st & (1ull << 46)) || (tag >> 47 & 7) != GS_SPRITE ||
            (st >> 47 & 0x7ff) != (tag >> 47 & 0x7ff) || word(f + 72) != 0x3f800000)
            return false;
        for (unsigned sprite = 0; sprite < 8; ++sprite)
        {
            const auto *uv = f + (7 + sprite * 4) * 16;
            const unsigned s = sprite & 1 ? 0x3f000000 : (sprite == 2 || sprite == 4 ? 0x3f800000 : 0);
            const unsigned t = sprite & 1 ? 0x3f000000 : (sprite == 4 || sprite == 6 ? 0x3f800000 : 0);
            if (word(uv) != s || word(uv + 4) != t || word(uv + 8) != 0x3f800000)
                return false;
        }
    }
    return true;
}

bool prepareGrassGroup(DualGeometry &pool, const GrassPacketView *views, uint32_t count, PreparedGrass *prepared,
                       GrassLiveStats &stats)
{
    const auto declined = [&] {
        ++stats.groupDeclined;
        return false;
    };
    if (!views || !prepared || count < 2 || count > 8 || !g_gs_device ||
        g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || g_gs_renderer->m_path[0].nloop ||
        GSRendererHW::GetInstance()->GetUpscaleMultiplier() <= 1.0f)
        return declined();
    std::array<PreparedGrass, 8> candidate;
    std::array<uint8_t, 8 * 320> inputs;
    std::array<GrassRasterState, 8> raster;
    const auto &rgba = GSRendererHW::GetInstance()->GetNativeVertexLatch().RGBAQ;
    uint32_t incoming[] = {rgba.R, rgba.G, rgba.B, rgba.A};
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &view = views[i];
        if (!view.geometry || !grassPacketFraming(*view.geometry, view.packet, view.bytes) ||
            !prepare(*view.geometry, view.packet, incoming, candidate[i], stats))
            return declined();
        std::memcpy(inputs.data() + i * 320, candidate[i].inputs.data(), 320);
        raster[i] = candidate[i].raster;
        // The final RGBA of field 1 is retained even in deferred packets.
        // GS latches packed color bytes, not the untruncated raw qword lanes.
        for (unsigned channel = 0; channel < 4; ++channel)
            incoming[channel] = word(view.packet + 74 * 16 + channel * 4) & 255;
    }
    const auto batch = pool.prepareGrass(inputs.data(), count, raster.data());
    if (!batch)
        return declined();
    for (unsigned i = 0; i < count; ++i)
    {
        candidate[i].batch = batch;
        candidate[i].firstSource = i * 16;
        prepared[i] = std::move(candidate[i]);
    }
    ++stats.groupedDispatches;
    stats.groupedPackets += count;
    return true;
}

bool drawGrassPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes,
                     GrassLiveStats &stats, bool forceFallback, const PreparedGrass *prepared)
{
    if (!grassPacketFraming(geometry, packet, bytes) || !g_gs_device ||
        g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || g_gs_renderer->m_path[0].nloop)
        return false;
    auto *renderer = GSRendererHW::GetInstance();
    if (renderer->GetUpscaleMultiplier() <= 1.0f)
        return false; // Native-resolution primitive culling uses another rule.
    PreparedGrass local;
    const auto &rgba = renderer->GetNativeVertexLatch().RGBAQ;
    const uint32_t incoming[] = {rgba.R, rgba.G, rgba.B, rgba.A};
    if (prepared && (prepared->geometry != &geometry || prepared->packet != packet || !prepared->batch ||
                     std::memcmp(prepared->inputs.data() + 288, incoming, 16)))
        prepared = nullptr;
    if (!prepared && !prepare(geometry, packet, incoming, local, stats))
    {
        ++stats.boundsDeclined;
        return false;
    }
    if (!prepared)
        prepared = &local;
    Draw draw{pool, geometry, packet, stats, forceFallback};
    draw.inputs = prepared->inputs;
    draw.raster = prepared->raster;
    draw.batch = prepared->batch;
    draw.firstSource = prepared->firstSource;
    draw.grouped = bool(prepared->batch);
    ++stats.packets;
    constexpr std::array<uint16_t, 16> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned field = 0; field < 2; ++field)
    {
        draw.field = field;
        const auto *f = packet + field * 608;
        // A+D state, then the original PRE/STQ tag. It establishes the same
        // PRIM as the geometry tag and consumes no deferred coordinates.
        kzgs::ExecuteGIFOnGSThread(1, f, 80);
        GSVertexTrace::Vertex minimum{}, maximum{};
        const bool consumed =
            varyingDepth(draw, field) &&
            prepareGrassTrace(prepared->bounds[field], *renderer->m_context, *renderer->PRIM, minimum, maximum) &&
            renderer->DrawNativeSprites(minimum, maximum, indices.data(), 16, true, &draw, submit, materialize);
        if (!consumed)
        {
            ++stats.declinedFields;
            kzgs::ExecuteGIFOnGSThread(1, fullPacket(draw) + field * 608 + 80, 528);
            continue;
        }
        // Reproduce STQ/RGBA/XYZ/FOG and the final four XY-history entries from
        // the resolved CPU pair, suppressing its primitives with ADC.
        restoreTail(f);
    }
    return true;
}
} // namespace tf_gpu
