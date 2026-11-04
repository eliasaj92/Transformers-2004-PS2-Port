// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live_grass.h"
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_dual_frontend.h"
#include "runtime/gs/gs_native_grass_math.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <array>
#include <cstring>
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
    std::array<uint8_t, 1216> restored;
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
    if (!draw.pool.draw(draw.batch, config, draw.field * 8, 8))
        return false;
    ++draw.stats.gpuDraws;
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
} // namespace

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

bool drawGrassPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes,
                     GrassLiveStats &stats, bool forceFallback)
{
    if (!grassPacketFraming(geometry, packet, bytes) || !g_gs_device ||
        g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || g_gs_renderer->m_path[0].nloop)
        return false;
    auto *renderer = GSRendererHW::GetInstance();
    if (renderer->GetUpscaleMultiplier() <= 1.0f)
        return false; // Native-resolution primitive culling uses another rule.
    Draw draw{pool, geometry, packet, stats, forceFallback};
    std::memcpy(draw.inputs.data(), geometry.inputs.data(), 256);
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *f = packet + field * 608;
        std::memcpy(draw.inputs.data() + 256 + field * 16, f + 64, 16);
        GIFRegSCISSOR scissor{};
        GIFRegXYOFFSET offset{};
        std::memcpy(&scissor, f + 16, 8);
        std::memcpy(&offset, f + 32, 8);
        if (scissor.SCAX0 > scissor.SCAX1 || scissor.SCAY0 > scissor.SCAY1)
            return false;
        draw.raster.scissor[field] = {int(scissor.SCAX0) * 16 - 8, int(scissor.SCAY0) * 16 - 8,
                                      int(scissor.SCAX1) * 16 + 8, int(scissor.SCAY1) * 16 + 8};
        draw.raster.offset[field] = {int(offset.OFX), int(offset.OFY), 0, 1};
    }
    const auto &rgba = renderer->GetNativeVertexLatch().RGBAQ;
    const uint32_t incoming[] = {rgba.R, rgba.G, rgba.B, rgba.A};
    std::memcpy(draw.inputs.data() + 288, incoming, 16);
    std::memcpy(draw.inputs.data() + 304, packet + 36 * 16, 16);
    std::array<DualDrawBounds, 2> bounds;
    if (!projectGrassBounds(draw.inputs.data(), bounds))
    {
        ++stats.boundsDeclined;
        return false;
    }
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
            prepareGrassTrace(bounds[field], *renderer->m_context, *renderer->PRIM, minimum, maximum) &&
            renderer->DrawNativeSprites(minimum, maximum, indices.data(), 16, true, &draw, submit, materialize);
        if (!consumed)
        {
            ++stats.declinedFields;
            kzgs::ExecuteGIFOnGSThread(1, fullPacket(draw) + field * 608 + 80, 528);
            continue;
        }
        // Reproduce STQ/RGBA/XYZ/FOG and the final four XY-history entries from
        // the resolved CPU pair, suppressing its primitives with ADC.
        alignas(16) std::array<uint8_t, 144> tail;
        std::memcpy(tail.data(), f + 80, 16);
        put(tail.data(), (word(tail.data()) & ~32767u) | 2);
        std::memcpy(tail.data() + 16, f + 30 * 16, 128);
        for (unsigned sprite = 0; sprite < 2; ++sprite)
            for (unsigned endpoint : {0u, 48u})
            {
                auto *flags = tail.data() + 16 + sprite * 64 + endpoint + 12;
                put(flags, word(flags) | 0x8000);
            }
        kzgs::ExecuteGIFOnGSThread(1, tail.data(), uint32_t(tail.size()));
    }
    return true;
}
} // namespace tf_gpu
