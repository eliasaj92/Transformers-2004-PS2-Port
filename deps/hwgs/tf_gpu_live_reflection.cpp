// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live_reflection.h"
#include "tf_gpu_dual_frontend.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace tf_gpu
{
namespace
{
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
void put(void *p, uint32_t v)
{
    std::memcpy(p, &v, 4);
}
struct Draw
{
    DualGeometry &pool;
    const gsnative::DualPacket &geometry;
    const uint8_t *packet;
    uint32_t bytes;
    ReflectionLiveStats &stats;
    bool forceFallback;
    DualGeometry::Batch batch;
    uint32_t firstSource = 0;
    ReflectionRasterState raster;
    alignas(16) std::array<uint8_t, 4 * (4 + 3 * gsnative::ReflectionMaxVertices) * 16> restored;
    bool hasRestored = false;
};
const uint8_t *fullPacket(Draw &draw)
{
    if (draw.geometry.version != 2)
        return draw.packet;
    if (!draw.hasRestored)
    {
        std::memcpy(draw.restored.data(), draw.packet, draw.bytes);
        gsnative::materializeReflectionPacket(draw.geometry, draw.restored.data(), draw.bytes);
        draw.hasRestored = true;
        ++draw.stats.materializations;
    }
    return draw.restored.data();
}
bool submit(void *context, GSHWDrawConfig &config)
{
    auto &d = *static_cast<Draw *>(context);
    if (d.forceFallback || config.nverts != (d.geometry.vertices - 2) * 12 ||
        !GSDevice12::GetInstance()->CanRenderHWFromGPU(config))
        return false;
    if (!d.batch)
    {
        const ReflectionInput strip{nullptr, d.geometry.vertices, d.raster, &d.geometry};
        d.batch = d.pool.prepareReflectionGroup(&strip, 1);
        if (!d.batch)
        {
            ++d.stats.outputDeclined;
            return false;
        }
    }
    // The existing pool counts pairs; reflection has four interleaved views.
    if (!d.pool.draw(d.batch, config, d.firstSource, (d.geometry.vertices - 2) * 6))
        return false;
    ++d.stats.gpuDraws;
    return true;
}
bool culled(const ReflectionRasterState &raster, unsigned section, const uint8_t *triangle)
{
    GSVector4i box;
    std::array<std::array<int, 2>, 3> points;
    for (unsigned corner = 0; corner < 3; ++corner)
    {
        const auto *xyz = triangle + corner * 48 + 32;
        const int x = int(word(xyz) & 65535) - raster.offset[section][0];
        const int y = int(word(xyz + 4) & 65535) - raster.offset[section][1];
        points[corner] = {x, y};
        const GSVector4i point(x, y, x, y);
        box = corner ? box.runion(point) : point;
    }
    box -= ((box & GSVector4i(15)) == GSVector4i(0)) & GSVector4i(0, 0, 1, 1);
    return box.rempty() ||
           !(box + GSVector4i(0, 0, 1, 1)).rintersects(GSVector4i::load<false>(raster.scissor[section].data())) ||
           points[0] == points[1] || points[1] == points[2] || points[0] == points[2];
}
void materialize(void *context, GSVertex *vertices, uint32_t count)
{
    auto &d = *static_cast<Draw *>(context);
    const unsigned n = d.geometry.vertices;
    if (count != (n - 2) * 12)
        throw std::runtime_error("Unexpected reflection fallback vertex count");
    const auto *packet = fullPacket(d);
    for (unsigned section = 0; section < 4; ++section)
        for (unsigned triangle = 0; triangle + 2 < n; ++triangle)
        {
            const auto *first = packet + (section * (4 + 3 * n) + 4 + triangle * 3) * 16;
            const bool skip = culled(d.raster, section, first);
            for (unsigned corner = 0; corner < 3; ++corner)
            {
                const auto *p = first + corner * 48;
                const auto *xyz = skip ? first : p;
                auto &v = vertices[(triangle * 3 + corner) * 4 + section];
                v = {};
                v.ST.U64 = wide(p);
                const uint32_t q = word(p + 8);
                v.RGBAQ.Q = std::bit_cast<float>(q ? q : 0x00800000u);
                v.RGBAQ.R = uint8_t(word(p + 16));
                v.RGBAQ.G = uint8_t(word(p + 20));
                v.RGBAQ.B = uint8_t(word(p + 24));
                v.RGBAQ.A = uint8_t(word(p + 28));
                v.XYZ.X = uint16_t(word(xyz + 32));
                v.XYZ.Y = uint16_t(word(xyz + 36));
                v.XYZ.Z = (word(xyz + 40) >> 4) & 0xffffff;
                v.FOG = (word(p + 44) >> 4) & 255;
            }
        }
}
bool varyingTail(const gsnative::DualPacket &geometry, const uint8_t *field, const ReflectionRasterState &raster,
                 unsigned section)
{
    // A completed triangle among the retained final three vertices must witness
    // nonconstant depth. No skipped geometry is calculated to establish this.
    // The 19-vertex fast tail retains three projection vertices, not four.
    const unsigned n = geometry.vertices, first = n - 3;
    for (unsigned i = first + 2; i < n; ++i)
    {
        const uint32_t mask = word(geometry.constants.data() + 132 + (i / 16) * 4);
        if (((mask << (i % 16)) & 32768) || culled(raster, section, field + 64 + (i - 2) * 48))
            continue;
        const auto *v = field + 64 + (i - 2) * 48 + 40;
        const auto z = (word(v) >> 4) & 0xffffff;
        if (((word(v + 48) >> 4) & 0xffffff) != z || ((word(v + 96) >> 4) & 0xffffff) != z)
            return true;
    }
    return false;
}
bool prepareBounds(const gsnative::DualPacket &p, const uint8_t *packet, PreparedReflection &prepared,
                   ReflectionLiveStats &stats)
{
    uint32_t nodes = 0, leaves = 0;
    const bool hasBounds = projectReflectionBounds(p, prepared.bounds, &nodes, &leaves);
    stats.boundsNodes += nodes;
    stats.boundsLeaves += leaves;
    if (!hasBounds)
    {
        ++stats.boundsDeclined;
        return false;
    }
    for (unsigned section = 0; section < 4; ++section)
    {
        const auto *f = packet + section * (4 + p.vertices * 3) * 16;
        GIFRegSCISSOR scissor{};
        GIFRegXYOFFSET offset{};
        std::memcpy(&scissor, f + 16, 8);
        std::memcpy(&offset, f + 32, 8);
        if (scissor.SCAX0 > scissor.SCAX1 || scissor.SCAY0 > scissor.SCAY1)
            return false;
        prepared.raster.scissor[section] = {int(scissor.SCAX0) * 16 - 8, int(scissor.SCAY0) * 16 - 8,
                                            int(scissor.SCAX1) * 16 + 8, int(scissor.SCAY1) * 16 + 8};
        prepared.raster.offset[section] = {int(offset.OFX), int(offset.OFY), 0, 0};
    }
    return true;
}
} // namespace

bool reflectionPacketFraming(const gsnative::DualPacket &p, const uint8_t *packet, uint32_t bytes)
{
    if (!packet || !gsnative::reflectionSourceValid(p) || bytes != 4 * (4 + p.vertices * 3) * 16)
        return false;
    for (unsigned section = 0; section < 4; ++section)
    {
        const auto *f = packet + section * (4 + 3 * p.vertices) * 16;
        const uint64_t state = wide(f), tag = wide(f + 48);
        const unsigned context = section / 2;
        if ((state & 32767) != 2 || (state >> 58 & 3) || state >> 60 != 1 || wide(f + 8) != 14 ||
            (tag & 32767) != p.vertices || (tag >> 58 & 3) || tag >> 60 != 3 || wide(f + 56) != 0x412 ||
            (tag >> 47 & 7) != 4 || !(tag & (1ull << 46)) || ((tag >> 56) & 1) != context ||
            bool(tag & 32768) != (section == 3) || wide(f + 24) != 0x40 + context || wide(f + 40) != 0x18 + context)
            return false;
    }
    return true;
}

bool prepareReflectionGroup(DualGeometry &pool, const ReflectionPacketView *packets, uint32_t count,
                            PreparedReflection *prepared, ReflectionLiveStats &stats)
{
    if (!packets || !prepared || !count || count > MaxReflectionComputeGroup || !g_gs_device ||
        g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || g_gs_renderer->m_path[0].nloop ||
        GSRendererHW::GetInstance()->GetUpscaleMultiplier() <= 1.0f)
        return false;
    std::array<ReflectionInput, MaxReflectionComputeGroup> strips;
    uint32_t first = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &view = packets[i];
        if (!view.geometry || !reflectionPacketFraming(*view.geometry, view.packet, view.bytes) ||
            !prepareBounds(*view.geometry, view.packet, prepared[i], stats))
        {
            ++stats.groupDeclined;
            return false;
        }
        prepared[i].firstSource = first;
        first += (view.geometry->vertices - 2) * 6;
        strips[i] = {nullptr, view.geometry->vertices, prepared[i].raster, view.geometry};
    }
    const auto batch = pool.prepareReflectionGroup(strips.data(), count);
    if (!batch)
    {
        ++stats.groupDeclined;
        ++stats.outputDeclined;
        return false;
    }
    for (unsigned i = 0; i < count; ++i)
        prepared[i].batch = batch;
    ++stats.preparedGroups;
    stats.preparedPackets += count;
    return true;
}

bool drawReflectionPacket(DualGeometry &pool, const gsnative::DualPacket &p, const uint8_t *packet, uint32_t bytes,
                          ReflectionLiveStats &stats, bool forceFallback, const PreparedReflection *prepared)
{
    if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || !reflectionPacketFraming(p, packet, bytes) ||
        g_gs_renderer->m_path[0].nloop || GSRendererHW::GetInstance()->GetUpscaleMultiplier() <= 1.0f)
        return false;
    ++stats.packets;
    PreparedReflection local;
    if (!prepared)
    {
        if (!prepareBounds(p, packet, local, stats))
            return false;
        prepared = &local;
    }
    Draw draw{pool, p, packet, bytes, stats, forceFallback};
    draw.raster = prepared->raster;
    draw.batch = prepared->batch;
    draw.firstSource = prepared->firstSource;
    auto *renderer = GSRendererHW::GetInstance();
    const unsigned sectionBytes = (4 + 3 * p.vertices) * 16;
    for (unsigned section = 0; section < 4; ++section)
    {
        const auto *f = packet + section * sectionBytes;
        alignas(16) std::array<uint64_t, 8> prefix;
        std::memcpy(prefix.data(), f, 48);
        prefix[0] = (prefix[0] & ~32767ull) | 3 | (1ull << 15);
        prefix[6] = (wide(f + 48) >> 47) & 0x7ff;
        prefix[7] = 0;
        kzgs::ExecuteGIFOnGSThread(1, reinterpret_cast<const uint8_t *>(prefix.data()), sizeof(prefix));
        std::array<uint16_t, (gsnative::ReflectionMaxVertices - 2) * 3> indices;
        unsigned nindices = 0;
        for (unsigned i = 2; i < p.vertices; ++i)
        {
            const uint32_t mask = word(p.constants.data() + 132 + (i / 16) * 4);
            if ((mask << (i % 16)) & 32768)
                continue;
            for (unsigned corner = 0; corner < 3; ++corner)
                indices[nindices++] = uint16_t(((i - 2) * 3 + corner) * 4 + section);
        }
        GSVertexTrace::Vertex minimum{}, maximum{};
        const bool consumed =
            !nindices ||
            (varyingTail(p, f, draw.raster, section) &&
             prepareDualTrace(prepared->bounds[section], *renderer->m_context, *renderer->PRIM, minimum, maximum) &&
             renderer->DrawNativeTriangles(minimum, maximum, indices.data(), nindices, (p.vertices - 2) * 12, true,
                                           &draw, submit, materialize));
        if (!consumed)
        {
            ++stats.declinedSections;
            kzgs::ExecuteGIFOnGSThread(1, fullPacket(draw) + section * sectionBytes, sectionBytes);
            continue;
        }
        alignas(16) std::array<uint8_t, 112> tail;
        std::memcpy(tail.data(), f + 48, 16);
        put(tail.data(), (word(tail.data()) & ~32767u) | 2);
        std::memcpy(tail.data() + 16, f + 64 + (p.vertices - 2) * 48, 96);
        for (unsigned i = 0; i < 2; ++i)
            put(tail.data() + 16 + i * 48 + 44, word(tail.data() + 16 + i * 48 + 44) | 0x8000);
        kzgs::ExecuteGIFOnGSThread(1, tail.data(), uint32_t(tail.size()));
    }
    return true;
}
} // namespace tf_gpu
