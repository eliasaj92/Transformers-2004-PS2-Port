// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_live_dual.h"
#include "tf_gpu_merge_guards.h"
#include "runtime/gs/gs_native_dual_math.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <list>
#include <unordered_map>
#include <stdexcept>
#include <vector>

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
} // namespace
bool dualPacketFraming(const gsnative::DualPacket &p, const uint8_t *packet, uint32_t bytes)
{
    if (!packet || p.kind != gsnative::GeometryKind::DualTransform || (p.version != 1 && p.version != 2) ||
        p.vertices < 3 || p.vertices > 28 || bytes != (8 + p.vertices * 6) * 16 || word(p.constants.data() + 140))
        return false;
    for (unsigned i = 0; i < p.vertices; ++i)
        if (word(p.inputs.data() + i * 32 + 12))
            return false;
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *f = packet + field * (4 + 3 * p.vertices) * 16;
        const uint64_t state = wide(f), tag = wide(f + 48);
        const unsigned context = (tag >> (47 + 9)) & 1;
        if ((state & 32767) != 2 || (state >> 58 & 3) || state >> 60 != 1 || wide(f + 8) != 14 ||
            (tag & 32767) != p.vertices || (tag >> 58 & 3) || tag >> 60 != 3 || wide(f + 56) != 0x412 ||
            (tag >> 47 & 7) != 4 || !(tag & (1ull << 46)) || wide(f + 24) != 0x40 + context ||
            wide(f + 40) != 0x18 + context)
            return false;
    }
    return true;
}
bool dualMeshTransformsMatch(const DualPacketView &a, const DualPacketView &b)
{
    return a.geometry && b.geometry &&
           !std::memcmp(a.geometry->constants.data(), b.geometry->constants.data(), 132);
}
bool dualMeshStateMatches(const DualPacketView &a, const DualPacketView &b)
{
    if (!a.geometry || !b.geometry || !a.packet || !b.packet) return false;
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *x = a.packet + field * (4 + a.geometry->vertices * 3) * 16;
        const auto *y = b.packet + field * (4 + b.geometry->vertices * 3) * 16;
        if (wide(x + 16) != wide(y + 16) || wide(x + 32) != wide(y + 32) ||
            ((wide(x + 48) >> 47) & 0x7ff) != ((wide(y + 48) >> 47) & 0x7ff)) return false;
    }
    return true;
}
struct DualMeshCache::Impl
{
    struct Entry
    {
        uint64_t hash = 0;
        std::vector<uint8_t> source;
        std::vector<uint32_t> counts;
        Prepared prepared;
    };
    DualGeometry &pool;
    std::list<Entry> entries;
    std::unordered_multimap<uint64_t, std::list<Entry>::iterator> lookup;
    explicit Impl(DualGeometry &p) : pool(p) {}
};
DualMeshCache::DualMeshCache(DualGeometry &pool) : impl(std::make_unique<Impl>(pool)) {}
DualMeshCache::~DualMeshCache() = default;
DualMeshCache::Prepared *DualMeshCache::prepare(const DualPacketView *views, uint32_t count, DualMergeStats &stats)
{
    if (!views || count < 2 || count > MaxDualMeshPackets)
        return nullptr;
    uint64_t key = 14695981039346656037ull;
    const auto hash = [&](const void *data, unsigned bytes) {
        const auto *p = static_cast<const uint8_t *>(data);
        for (unsigned i = 0; i < bytes; ++i) key = (key ^ p[i]) * 1099511628211ull;
    };
    for (unsigned i = 0; i < count; ++i)
    {
        if (!views[i].geometry || !dualPacketFraming(*views[i].geometry, views[i].packet, views[i].bytes))
            return nullptr;
        const auto &p = *views[i].geometry;
        hash(&p.vertices, sizeof(p.vertices));
        hash(p.inputs.data(), p.vertices * 32);
        hash(p.constants.data() + 132, 8); // ADC strip restarts are part of topology.
    }
    auto &c = *impl;
    auto entry = c.entries.end();
    const auto [begin, end] = c.lookup.equal_range(key);
    for (auto it = begin; it != end; ++it)
    {
        auto candidate = it->second;
        if (candidate->counts.size() != count) continue;
        bool equal = true;
        unsigned offset = 0;
        for (unsigned i = 0; equal && i < count; ++i)
        {
            const auto &p = *views[i].geometry;
            equal = candidate->counts[i] == p.vertices &&
                    !std::memcmp(candidate->source.data() + offset, p.inputs.data(), p.vertices * 32) &&
                    !std::memcmp(candidate->prepared.constants.data() + i * 144 + 132,
                                 p.constants.data() + 132, 8);
            offset += p.vertices * 32;
        }
        if (equal) { entry = candidate; ++stats.cacheHits; break; }
    }
    if (entry == c.entries.end())
    {
        // Bounded independently of the transient per-chunk cache. The pool
        // retires released buffers only after their last GPU fence completes.
        while (c.entries.size() >= 512)
        {
            const auto old = std::prev(c.entries.end());
            const auto range = c.lookup.equal_range(old->hash);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == old) { c.lookup.erase(it); break; }
            c.entries.erase(old);
        }
        Impl::Entry fresh;
        fresh.hash = key;
        fresh.prepared.constants.resize(count * 144);
        std::vector<uint8_t> upload;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto &p = *views[i].geometry;
            const unsigned first = fresh.prepared.vertices;
            fresh.counts.push_back(p.vertices);
            fresh.source.insert(fresh.source.end(), p.inputs.data(), p.inputs.data() + p.vertices * 32);
            upload.insert(upload.end(), p.inputs.data(), p.inputs.data() + p.vertices * 32);
            for (unsigned v = 0; v < p.vertices; ++v) put(upload.data() + (first + v) * 32 + 12, i);
            auto *record = fresh.prepared.constants.data() + i * 144;
            std::memcpy(record + 132, p.constants.data() + 132, 8);
            put(record + 140, first);
            bool hasTriangles = false;
            for (unsigned v = 2; v < p.vertices; ++v)
            {
                if ((word(record + 132 + (v / 16) * 4) << (v % 16)) & 32768) continue;
                hasTriangles = true;
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned corner = v - 2; corner <= v; ++corner)
                        fresh.prepared.indices[field].push_back(uint16_t(2 * (first + corner) + field));
            }
            fresh.prepared.triangleChunks.push_back(hasTriangles);
            fresh.prepared.vertices += p.vertices;
        }
        fresh.prepared.mesh = c.pool.uploadMesh(upload.data(), fresh.prepared.vertices, count);
        if (!fresh.prepared.mesh) return nullptr;
        c.entries.push_front(std::move(fresh));
        entry = c.entries.begin();
        c.lookup.emplace(key, entry);
        ++stats.cacheUploads;
    }
    c.entries.splice(c.entries.begin(), c.entries, entry);
    auto &result = entry->prepared;
    for (unsigned i = 0; i < count; ++i)
        std::memcpy(result.constants.data() + i * 144, views[i].geometry->constants.data(), 132);
    for (unsigned first = 0; first < count;)
    {
        unsigned end = first + 1;
        while (end < count && dualMeshTransformsMatch(views[first], views[end])) ++end;
        std::array<DualDrawBounds, 2> bounds;
        if (!c.pool.bounds(result.mesh, result.constants.data(), count, first, end - first, bounds)) return nullptr;
        if (!first) result.bounds = bounds;
        else for (unsigned f = 0; f < 2; ++f) mergeDrawBounds(result.bounds[f], bounds[f]);
        first = end;
    }
    return &result;
}
namespace
{
struct MergedDraw
{
    DualGeometry &pool;
    const DualPacketView *views;
    uint32_t count, vertices;
    DualMergeStats &stats;
    bool forceFallback;
    DualGeometry::Batch batch;
};
bool submit(void *context, GSHWDrawConfig &config)
{
    auto &draw = *static_cast<MergedDraw *>(context);
    if (draw.forceFallback || config.nverts != draw.vertices * 2 ||
        !draw.pool.draw(draw.batch, config, 0, draw.vertices))
        return false;
    ++draw.stats.gpuDraws;
    return true;
}
void materialize(void *context, GSVertex *vertices, uint32_t count)
{
    auto &draw = *static_cast<MergedDraw *>(context);
    if (count != draw.vertices * 2)
        throw std::runtime_error("Unexpected merged dual fallback vertex count");
    ++draw.stats.materializations;
    unsigned first = 0;
    for (unsigned packet = 0; packet < draw.count; ++packet)
    {
        const auto &view = draw.views[packet];
        const unsigned n = view.geometry->vertices;
        alignas(16) std::array<uint8_t, (8 + 28 * 6) * 16> restored;
        std::memcpy(restored.data(), view.packet, view.bytes);
        gsnative::materializeDualPacket(*view.geometry, restored.data(), view.bytes);
        for (unsigned field = 0; field < 2; ++field)
            for (unsigned i = 0; i < n; ++i)
            {
                const auto *p = restored.data() + (field * (4 + 3 * n) + 4 + i * 3) * 16;
                auto &v = vertices[2 * (first + i) + field];
                v = {};
                v.ST.U64 = wide(p);
                const uint32_t q = word(p + 8);
                v.RGBAQ.Q = std::bit_cast<float>(q ? q : 0x00800000u);
                v.RGBAQ.R = uint8_t(word(p + 16));
                v.RGBAQ.G = uint8_t(word(p + 20));
                v.RGBAQ.B = uint8_t(word(p + 24));
                v.RGBAQ.A = uint8_t(word(p + 28));
                v.XYZ.X = uint16_t(word(p + 32));
                v.XYZ.Y = uint16_t(word(p + 36));
                v.XYZ.Z = (word(p + 40) >> 4) & 0xffffff;
                v.FOG = (word(p + 44) >> 4) & 255;
            }
        first += n;
    }
}
void prefix(const uint8_t *field)
{
    alignas(16) std::array<uint64_t, 8> packet;
    std::memcpy(packet.data(), field, 48);
    packet[0] = (packet[0] & ~32767ull) | 3 | (1ull << 15);
    packet[6] = (wide(field + 48) >> 47) & 0x7ff;
    packet[7] = 0;
    kzgs::ExecuteGIFOnGSThread(1, reinterpret_cast<const uint8_t *>(packet.data()), sizeof(packet));
}
void restoreTail(const DualPacketView &view, unsigned field)
{
    const unsigned n = view.geometry->vertices;
    const auto *f = view.packet + field * (4 + 3 * n) * 16;
    alignas(16) std::array<uint8_t, 112> tail;
    std::memcpy(tail.data(), f + 48, 16);
    put(tail.data(), (word(tail.data()) & ~32767u) | 2);
    std::memcpy(tail.data() + 16, f + 64 + (n - 2) * 48, 96);
    for (unsigned i = 0; i < 2; ++i)
        put(tail.data() + 16 + i * 48 + 44, word(tail.data() + 16 + i * 48 + 44) | 0x8000);
    kzgs::ExecuteGIFOnGSThread(1, tail.data(), uint32_t(tail.size()));
}
void ordinaryField(MergedDraw &draw, unsigned field)
{
    for (unsigned i = 0; i < draw.count; ++i)
    {
        const auto &view = draw.views[i];
        alignas(16) std::array<uint8_t, (8 + 28 * 6) * 16> restored;
        std::memcpy(restored.data(), view.packet, view.bytes);
        gsnative::materializeDualPacket(*view.geometry, restored.data(), view.bytes);
        const unsigned bytes = (4 + view.geometry->vertices * 3) * 16;
        kzgs::ExecuteGIFOnGSThread(1, restored.data() + field * bytes, bytes);
    }
    draw.stats.ordinaryFields += draw.count;
}
} // namespace

bool drawDualGroup(DualGeometry &pool, const DualPacketView *views, const PreparedDual *prepared, uint32_t count,
                   DualMergeStats &stats, bool forceFallback, DualMeshCache *cache)
{
    const auto decline = [](uint64_t &reason) {
        ++reason;
        return false;
    };
    if (!views || (!prepared && !cache) || count < 2 || count > (cache ? MaxDualMeshPackets : 8u) || !g_gs_device ||
        g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || g_gs_renderer->m_path[0].nloop ||
        GSRendererHW::GetInstance()->GetUpscaleMultiplier() <= 1.0f)
        return decline(stats.stateDeclined);
    auto *renderer = GSRendererHW::GetInstance();
    if (!renderer->m_env.PRMODECONT.AC || renderer->m_env.SCANMSK.MSK)
        return decline(stats.stateDeclined);
    for (unsigned i = 0; i < count; ++i)
        if (!views[i].geometry || !dualPacketFraming(*views[i].geometry, views[i].packet, views[i].bytes) ||
            (!cache && !prepared[i].mesh))
            return decline(stats.stateDeclined);
    const auto &first = views[0];
    GIFRegPRIM prim{};
    prim.U64 = (wide(first.packet + 48) >> 47) & 0x7ff;
    if (!prim.TME || !prim.IIP || prim.FST || prim.AA1)
        return decline(stats.stateDeclined);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned field = 0; field < 2; ++field)
        {
            const auto *a = first.packet + field * (4 + first.geometry->vertices * 3) * 16;
            const auto *b = views[i].packet + field * (4 + views[i].geometry->vertices * 3) * 16;
            if (wide(a + 16) != wide(b + 16) || wide(a + 32) != wide(b + 32) ||
                ((wide(b + 48) >> 47) & 0x7ff) != prim.U64)
                return decline(stats.stateDeclined);
        }
    GIFRegSCISSOR scissor[2]{};
    for (unsigned field = 0; field < 2; ++field)
        std::memcpy(&scissor[field], first.packet + field * (4 + first.geometry->vertices * 3) * 16 + 16, 8);
    for (const auto &s : scissor)
        if (s.SCAX0 > s.SCAX1 || s.SCAY0 > s.SCAY1)
            return decline(stats.stateDeclined);
    if (!(scissor[0].SCAX1 < scissor[1].SCAX0 || scissor[1].SCAX1 < scissor[0].SCAX0 ||
          scissor[0].SCAY1 < scissor[1].SCAY0 || scissor[1].SCAY1 < scissor[0].SCAY0))
        return decline(stats.stateDeclined);
    const auto &context = renderer->m_env.CTXT[prim.CTXT];
    if (!noFieldTextureFeedback(context, scissor[0], scissor[1]))
        return decline(stats.feedbackDeclined);
    auto *resident = cache ? cache->prepare(views, count, stats) : nullptr;
    if (cache && !resident) return decline(stats.boundsDeclined);
    // Do not widen the vertex alpha range seen by blend selection. Mixed alpha
    // batches can select different hardware/software blending and rounding.
    if (prim.ABE)
    {
        const unsigned alpha = resident ? resident->bounds[0].colorMin[3] : prepared[0].bounds[0].colorMin[3];
        for (unsigned i = 0; i < (resident ? 1u : count); ++i)
            for (const auto &b : resident ? resident->bounds : prepared[i].bounds)
                if (b.colorMin[3] != alpha || b.colorMax[3] != alpha)
                    return decline(stats.stateDeclined);
    }
    std::array<DualGeometry::Mesh, 8> meshes;
    std::array<uint8_t, 8 * 144> constants;
    std::array<DualDrawBounds, 2> bounds;
    std::array<std::array<uint16_t, 8 * 26 * 3>, 2> indices;
    const uint16_t *drawIndices[2]{indices[0].data(), indices[1].data()};
    unsigned nindices[2]{}, vertices = 0;
    bool witness[2]{};
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &geometry = *views[i].geometry;
        if (!resident)
        {
            meshes[i] = prepared[i].mesh;
            std::memcpy(constants.data() + i * 144, geometry.constants.data(), 144);
        }
        bool hasTriangles = resident ? resident->triangleChunks[i] : false;
        for (unsigned vertex = 2; !resident && vertex < geometry.vertices; ++vertex)
        {
            const uint32_t mask = word(geometry.constants.data() + 132 + (vertex / 16) * 4);
            if ((mask << (vertex % 16)) & 32768)
                continue;
            hasTriangles = true;
            if (!resident) for (unsigned field = 0; field < 2; ++field)
                for (unsigned corner = vertex - 2; corner <= vertex; ++corner)
                    indices[field][nindices[field]++] = uint16_t(2 * (vertices + corner) + field);
        }
        for (unsigned field = 0; field < 2; ++field)
        {
            witness[field] |= hasTriangles && (geometry.varyingDepth & (1u << field));
            if (resident) continue;
            if (i)
                mergeDrawBounds(bounds[field], prepared[i].bounds[field]);
            else
                bounds[field] = prepared[i].bounds[field];
        }
        vertices += geometry.vertices;
    }
    if (resident)
    {
        bounds = resident->bounds;
        for (unsigned f = 0; f < 2; ++f)
        {
            drawIndices[f] = resident->indices[f].data();
            nindices[f] = uint32_t(resident->indices[f].size());
        }
    }
    GSVertexTrace::Vertex minimum[2]{}, maximum[2]{};
    for (unsigned field = 0; field < 2; ++field)
    {
        auto state = context;
        state.SCISSOR = scissor[field];
        std::memcpy(&state.XYOFFSET, first.packet + field * (4 + first.geometry->vertices * 3) * 16 + 32, 8);
        state.UpdateScissor();
        if (!nindices[field] || !witness[field] ||
            !prepareDualTrace(bounds[field], state, prim, minimum[field], maximum[field]))
            return decline(stats.boundsDeclined);
    }
    auto batch = resident ? pool.prepare(resident->mesh, resident->constants.data(), count)
                          : pool.prepareGroup(meshes.data(), constants.data(), count);
    if (!batch)
        return decline(stats.outputDeclined);
    MergedDraw draw{pool, views, count, vertices, stats, forceFallback, std::move(batch)};
    ++stats.groups;
    stats.packets += count;
    if (resident)
    {
        ++stats.nativeBatches;
        stats.nativePackets += count;
        stats.largestBatch = std::max(stats.largestBatch, uint64_t(count));
    }
    // Original triangle order is retained within each independent field.
    // Local indices never connect two source strips, including their ADC cuts.
    for (unsigned field = 0; field < 2; ++field)
    {
        prefix(first.packet + field * (4 + first.geometry->vertices * 3) * 16);
        if (renderer->DrawNativeTriangles(minimum[field], maximum[field], drawIndices[field], nindices[field],
                                          vertices * 2, true, &draw, submit, materialize))
            restoreTail(views[count - 1], field);
        else
            ordinaryField(draw, field);
    }
    return true;
}
} // namespace tf_gpu
