// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_hwgs.h"
#include "tf_gpu_live_draw.h"
#include "tf_gpu_live_grass.h"
#include "tf_gpu_live_dual.h"
#include "tf_gpu_live_reflection.h"
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_dual_frontend.h"
#include "runtime/gs/gs_native_grass_math.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
struct alignas(16) Payload
{
    gsnative::DualPacket geometry;
    uint32_t path, bytes;
    uint64_t reserved = 0;
};
struct CacheEntry
{
    uint64_t hash;
    uint32_t vertices;
    std::array<uint8_t, 28 * 32> inputs;
    tf_gpu::DualGeometry::Mesh mesh;
};
struct Live
{
    tf_gpu::DualGeometry pool;
    std::list<CacheEntry> meshes;
    std::unordered_multimap<uint64_t, std::list<CacheEntry>::iterator> lookup;
    std::unordered_set<uint64_t> sourceHashes;
    uint64_t packets = 0, gpuDraws = 0, cpuFallbacks = 0, declinedFields = 0, sourceHits = 0, noBounds = 0;
    uint64_t sourceDeclined = 0, outputDeclined = 0;
    uint64_t packetGroups = 0, groupPackets = 0, groupDeclined = 0;
    tf_gpu::GrassLiveStats grass;
    tf_gpu::DualMergeStats merged;
    tf_gpu::ReflectionLiveStats reflection;
    // Retain the visible level's source working set. The initial 256-entry
    // cache uploaded 125,208 of 155,337 packets. Extra pool capacity covers
    // released buffers which are still in flight.
    explicit Live(GSDevice12 &device) : pool(device, 128ull * 1024 * 1024, 192ull * 1024 * 1024)
    {
    }
    tf_gpu::DualGeometry::Mesh source(const gsnative::DualPacket &p)
    {
        const uint64_t key = hash(p.inputs.data(), p.vertices * 32);
        if (sourceHashes.size() < 65536)
            sourceHashes.insert(key);
        const auto [first, last] = lookup.equal_range(key);
        for (auto it = first; it != last; ++it)
        {
            auto entry = it->second;
            if (entry->vertices == p.vertices && !std::memcmp(entry->inputs.data(), p.inputs.data(), p.vertices * 32))
            {
                ++sourceHits;
                meshes.splice(meshes.begin(), meshes, entry);
                return entry->mesh;
            }
        }
        while (meshes.size() >= 2048)
        {
            const auto old = std::prev(meshes.end());
            const auto range = lookup.equal_range(old->hash);
            for (auto it = range.first; it != range.second; ++it)
                if (it->second == old)
                {
                    lookup.erase(it);
                    break;
                }
            meshes.erase(old);
        }
        auto mesh = pool.uploadMesh(p.inputs.data(), p.vertices, 1);
        if (mesh)
        {
            meshes.push_front({key, p.vertices, p.inputs, mesh});
            lookup.emplace(key, meshes.begin());
        }
        return mesh;
    }
    void report() const
    {
        const auto &s = pool.stats();
        std::fprintf(stderr,
                     "[gpu-reflection:gs] packets=%llu gpu_draws=%llu cpu_materializations=%llu "
                     "declined_sections=%llu bounds_declined=%llu output_declined=%llu "
                     "bounds_nodes=%llu bounds_leaves=%llu dispatches=%llu vertices=%llu\n",
                     reflection.packets, reflection.gpuDraws, reflection.materializations,
                     reflection.declinedSections, reflection.boundsDeclined, reflection.outputDeclined,
                     reflection.boundsNodes, reflection.boundsLeaves, s.reflectionDispatches, s.reflectionVertices);
        std::fprintf(stderr,
                     "[gpu-dual:gs] packets=%llu gpu_draws=%llu cpu_materializations=%llu declined_fields=%llu "
                     "source_hits=%llu bounds_declined=%llu dispatches=%llu source_uploads=%llu source_bytes=%llu "
                     "output_allocations=%llu page_probes=%llu source_declined=%llu output_declined=%llu "
                     "unique_source_hashes=%zu grouped_dispatches=%llu grouped_meshes=%llu packet_groups=%llu "
                     "group_packets=%llu group_declined=%llu merged_groups=%llu merged_packets=%llu "
                     "merged_draws=%llu merge_state_declined=%llu merge_feedback_declined=%llu "
                     "merge_bounds_declined=%llu merge_output_declined=%llu\n",
                     packets + merged.packets, gpuDraws + merged.gpuDraws, cpuFallbacks + merged.materializations,
                     declinedFields + merged.ordinaryFields, sourceHits, noBounds, s.dispatches, s.meshUploads,
                     s.sourceUploadBytes, s.allocations, s.pageProbes, sourceDeclined, outputDeclined,
                     sourceHashes.size(), s.groupedDispatches, s.groupedMeshes, packetGroups, groupPackets,
                     groupDeclined, merged.groups, merged.packets, merged.gpuDraws, merged.stateDeclined,
                     merged.feedbackDeclined, merged.boundsDeclined, merged.outputDeclined);
        std::fprintf(stderr,
                     "[gpu-grass:gs] packets=%llu gpu_draws=%llu cpu_materializations=%llu "
                     "declined_fields=%llu bounds_declined=%llu output_declined=%llu dispatches=%llu batches=%llu "
                     "grouped_dispatches=%llu grouped_packets=%llu grouped_draws=%llu group_declined=%llu "
                     "merged_groups=%llu merged_packets=%llu merged_draws=%llu merge_declined=%llu "
                     "merge_state_declined=%llu merge_feedback_declined=%llu merge_bounds_declined=%llu "
                     "bounds_queries=%llu bounds_cache_hits=%llu bounds_cache_rejected_hits=%llu largest_merge=%llu\n",
                     grass.packets, grass.gpuDraws, grass.materializations, grass.declinedFields, grass.boundsDeclined,
                     grass.outputDeclined, s.grassDispatches, s.grassBatches, grass.groupedDispatches,
                     grass.groupedPackets, grass.groupedDraws, grass.groupDeclined, grass.mergedGroups,
                     grass.mergedPackets, grass.mergedDraws, grass.mergeDeclined, grass.mergeStateDeclined,
                     grass.mergeFeedbackDeclined, grass.mergeBoundsDeclined, grass.boundsQueries, grass.boundsCacheHits,
                     grass.boundsCacheRejectedHits, grass.largestMerge);
    }
};
// Accessed/destroyed only on the owning GS thread, before device shutdown.
std::unique_ptr<Live> live;
void decodeField(const uint8_t *packet, unsigned vertices, unsigned field, GSVertex *output)
{
    const auto *f = packet + field * (4 + 3 * vertices) * 16;
    for (unsigned i = 0; i < vertices; ++i)
    {
        const auto *p = f + 64 + i * 48;
        GSVertex &v = output[2 * i + field];
        v = {};
        v.ST.U64 = wide(p);
        uint32_t q = word(p + 8);
        if (!q)
            q = 0x00800000;
        v.RGBAQ.Q = std::bit_cast<float>(q);
        v.RGBAQ.R = uint8_t(word(p + 16));
        v.RGBAQ.G = uint8_t(word(p + 20));
        v.RGBAQ.B = uint8_t(word(p + 24));
        v.RGBAQ.A = uint8_t(word(p + 28));
        v.XYZ.X = uint16_t(word(p + 32));
        v.XYZ.Y = uint16_t(word(p + 36));
        v.XYZ.Z = (word(p + 40) >> 4) & 0xffffff;
        v.FOG = (word(p + 44) >> 4) & 255;
    }
}
struct Draw
{
    Live &owner;
    const tf_gpu::DualGeometry::Mesh &mesh;
    const uint8_t *constants;
    tf_gpu::DualGeometry::Batch batch;
    const uint8_t *packet;
    unsigned vertices;
    const gsnative::DualPacket &geometry;
    std::vector<uint8_t> restored;
    uint32_t firstSource = 0;
};
const uint8_t *fullPacket(Draw &draw)
{
    if (draw.geometry.version != 2)
        return draw.packet;
    if (draw.restored.empty())
    {
        const unsigned bytes = (8 + 6 * draw.vertices) * 16;
        draw.restored.assign(draw.packet, draw.packet + bytes);
        gsnative::materializeDualPacket(draw.geometry, draw.restored.data(), bytes);
    }
    return draw.restored.data();
}
bool submit(void *context, GSHWDrawConfig &config)
{
    auto &d = *static_cast<Draw *>(context);
    if (config.nverts != d.vertices * 2 || !GSDevice12::GetInstance()->CanRenderHWFromGPU(config))
        return false;
    // Do not dispatch compute for culled draws or frontend cases which already
    // chose CPU materialization. Both fields share this one prepared output.
    if (!d.batch)
    {
        d.batch = d.owner.pool.prepare(d.mesh, d.constants, 1);
        if (!d.batch)
        {
            ++d.owner.outputDeclined;
            return false;
        }
    }
    if (!d.owner.pool.draw(d.batch, config, d.firstSource, d.vertices))
        return false;
    ++d.owner.gpuDraws;
    return true;
}
void materialize(void *context, GSVertex *vertices, uint32_t)
{
    auto &d = *static_cast<Draw *>(context);
    ++d.owner.cpuFallbacks;
    const auto *packet = fullPacket(d);
    decodeField(packet, d.vertices, 0, vertices);
    decodeField(packet, d.vertices, 1, vertices);
}
void executePacket(const uint8_t *bytes, uint32_t size, const tf_gpu::PreparedDual *prepared,
                   const tf_gpu::PreparedGrass *grassPrepared = nullptr)
{
    if (size < sizeof(Payload))
        return;
    const auto &payload = *reinterpret_cast<const Payload *>(bytes);
    const auto &p = payload.geometry;
    const auto *packet = bytes + sizeof(Payload);
    if (payload.bytes != size - sizeof(Payload))
        return;
    const auto ordinary = [&] {
        if (p.version != 2)
        {
            kzgs::ExecuteGIFOnGSThread(payload.path, packet, payload.bytes);
            return;
        }
        std::vector<uint8_t> restored(packet, packet + payload.bytes);
        gsnative::materializePacket(p, restored.data(), payload.bytes);
        kzgs::ExecuteGIFOnGSThread(payload.path, restored.data(), payload.bytes);
    };
    if (p.kind == gsnative::GeometryKind::GrassSprites)
    {
        if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12 && payload.path == 1)
        {
            if (!live)
                live = std::make_unique<Live>(*GSDevice12::GetInstance());
            if (tf_gpu::drawGrassPacket(live->pool, p, packet, payload.bytes, live->grass, false, grassPrepared))
                return;
        }
        ordinary();
        return;
    }
    if (p.kind == gsnative::GeometryKind::Reflection)
    {
        if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12 && payload.path == 1)
        {
            if (!live)
                live = std::make_unique<Live>(*GSDevice12::GetInstance());
            if (tf_gpu::drawReflectionPacket(live->pool, p, packet, payload.bytes, live->reflection))
                return;
        }
        ordinary();
        return;
    }
    if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12 || payload.path != 1 ||
        !tf_gpu::dualPacketFraming(p, packet, payload.bytes) || g_gs_renderer->m_path[0].nloop)
    {
        ordinary();
        return;
    }
    if (!live)
        live = std::make_unique<Live>(*GSDevice12::GetInstance());
    ++live->packets;
    auto mesh = prepared ? prepared->mesh : live->source(p);
    if (!mesh)
    {
        ++live->sourceDeclined;
        ordinary();
        return;
    }
    std::array<tf_gpu::DualDrawBounds, 2> bounds{};
    if (prepared)
        bounds = prepared->bounds;
    else if (!live->pool.bounds(mesh, p.constants.data(), 1, 0, 1, bounds))
    {
        ++live->noBounds;
        ordinary();
        return;
    }
    auto *renderer = GSRendererHW::GetInstance();
    const unsigned fieldBytes = (4 + 3 * p.vertices) * 16;
    Draw draw{*live, mesh, p.constants.data(), {}, packet, p.vertices, p, {}};
    if (prepared && prepared->batch)
    {
        draw.batch = prepared->batch;
        draw.firstSource = prepared->first;
    }
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *f = packet + field * fieldBytes;
        // Ordinary A+D state plus explicit PRIM from the geometry tag. No
        // transformed vertices enter GS draw preparation on the native path.
        alignas(16) std::array<uint64_t, 8> prefix;
        std::memcpy(prefix.data(), f, 48);
        prefix[0] = (prefix[0] & ~32767ull) | 3 | (1ull << 15);
        prefix[6] = (wide(f + 48) >> 47) & 0x7ff;
        prefix[7] = 0;
        kzgs::ExecuteGIFOnGSThread(1, reinterpret_cast<const uint8_t *>(prefix.data()), sizeof(prefix));
        std::array<uint16_t, 26 * 3> indices;
        unsigned nindices = 0;
        for (unsigned i = 2; i < p.vertices; ++i)
        {
            const uint32_t mask = word(p.constants.data() + 132 + (i / 16) * 4);
            if ((mask << (i % 16)) & 32768)
                continue;
            for (unsigned corner = i - 2; corner <= i; ++corner)
                indices[nindices++] = uint16_t(corner * 2 + field);
        }
        GSVertexTrace::Vertex minimum{}, maximum{};
        const bool consumed =
            !nindices ||
            ((p.varyingDepth & (1u << field)) &&
             tf_gpu::prepareDualTrace(bounds[field], *renderer->m_context, *renderer->PRIM, minimum, maximum) &&
             renderer->DrawNativeTriangles(minimum, maximum, indices.data(), nindices, p.vertices * 2, true, &draw,
                                           submit, materialize));
        if (!consumed)
        {
            ++live->declinedFields;
            kzgs::ExecuteGIFOnGSThread(1, fullPacket(draw) + field * fieldBytes, fieldBytes);
            continue;
        }
        // Reproduce the actual final STQ/RGBA/XYZ/FOG latches and the last two
        // strip vertices without drawing them again. ADC is not a GS attribute.
        alignas(16) std::array<uint8_t, 16 + 2 * 48> tail;
        std::memcpy(tail.data(), f + 48, 16);
        put(tail.data(), (word(tail.data()) & ~32767u) | 2);
        std::memcpy(tail.data() + 16, f + 64 + (p.vertices - 2) * 48, 96);
        for (unsigned i = 0; i < 2; ++i)
            put(tail.data() + 16 + i * 48 + 44, word(tail.data() + 16 + i * 48 + 44) | 0x8000);
        kzgs::ExecuteGIFOnGSThread(1, tail.data(), uint32_t(tail.size()));
    }
    if ((live->packets & 4095) == 0)
        live->report();
}
void execute(const uint8_t *bytes, uint32_t size)
{
    executePacket(bytes, size, nullptr);
}
void executeGroup(const kzgs::PacketView *views, uint32_t count)
{
    const auto ordinary = [&] {
        for (unsigned i = 0; i < count; ++i)
            execute(views[i].data, views[i].bytes);
    };
    constexpr unsigned maxComputeGroup = 8;
    if (count < 2 || count > maxComputeGroup || !g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12 ||
        g_gs_renderer->m_path[0].nloop)
    {
        ordinary();
        return;
    }
    bool grass = true, triangles = true;
    for (unsigned i = 0; i < count; ++i)
    {
        if (views[i].bytes < sizeof(Payload))
        {
            ordinary();
            return;
        }
        const auto &p = *reinterpret_cast<const Payload *>(views[i].data);
        if (p.path != 1 || p.bytes != views[i].bytes - sizeof(Payload) ||
            (!tf_gpu::dualPacketFraming(p.geometry, views[i].data + sizeof(Payload), p.bytes) &&
             !tf_gpu::grassPacketFraming(p.geometry, views[i].data + sizeof(Payload), p.bytes)))
        {
            ordinary();
            return;
        }
        grass &= p.geometry.kind == gsnative::GeometryKind::GrassSprites;
        triangles &= p.geometry.kind == gsnative::GeometryKind::DualTransform;
    }
    if (!grass && !triangles)
    {
        ordinary();
        return;
    }
    if (!live)
        live = std::make_unique<Live>(*GSDevice12::GetInstance());
    if (grass)
    {
        std::array<tf_gpu::GrassPacketView, maxComputeGroup> packets;
        std::array<tf_gpu::PreparedGrass, maxComputeGroup> prepared;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto &payload = *reinterpret_cast<const Payload *>(views[i].data);
            packets[i] = {&payload.geometry, views[i].data + sizeof(Payload), payload.bytes};
        }
        static const bool merge = [] {
            const char *option = std::getenv("PS2X_TF_GPU_GRASS_MERGE");
            return option && !std::strcmp(option, "1");
        }();
        if (merge && tf_gpu::drawGrassGroup(live->pool, packets.data(), count, live->grass))
            return;
        if (!tf_gpu::prepareGrassGroup(live->pool, packets.data(), count, prepared.data(), live->grass))
        {
            ordinary();
            return;
        }
        for (unsigned i = 0; i < count; ++i)
            executePacket(views[i].data, views[i].bytes, nullptr, &prepared[i]);
        return;
    }
    ++live->packetGroups;
    live->groupPackets += count;
    std::array<tf_gpu::PreparedDual, maxComputeGroup> prepared;
    std::array<tf_gpu::DualPacketView, maxComputeGroup> packets;
    std::array<tf_gpu::DualGeometry::Mesh, maxComputeGroup> meshes;
    std::array<uint8_t, maxComputeGroup * 144> constants;
    uint32_t first = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto &geometry = reinterpret_cast<const Payload *>(views[i].data)->geometry;
        packets[i] = {&geometry, views[i].data + sizeof(Payload), views[i].bytes - uint32_t(sizeof(Payload))};
        prepared[i].mesh = meshes[i] = live->source(geometry);
        if (!meshes[i] || !live->pool.bounds(meshes[i], geometry.constants.data(), 1, 0, 1, prepared[i].bounds))
        {
            ++live->groupDeclined;
            ordinary();
            return;
        }
        std::memcpy(constants.data() + i * 144, geometry.constants.data(), 144);
        prepared[i].first = first;
        first += geometry.vertices;
    }
    static const bool merge = [] {
        const char *option = std::getenv("PS2X_TF_GPU_DUAL_MERGE");
        return option && !std::strcmp(option, "1");
    }();
    if (merge && tf_gpu::drawDualGroup(live->pool, packets.data(), prepared.data(), count, live->merged))
        return;
    // Only immutable source buffers and owned output are touched here. All GS
    // state, textures, fallback handling and draw calls remain in packet order.
    const auto batch = live->pool.prepareGroup(meshes.data(), constants.data(), count);
    if (!batch)
        ++live->groupDeclined;
    for (unsigned i = 0; i < count; ++i)
    {
        prepared[i].batch = batch;
        executePacket(views[i].data, views[i].bytes, &prepared[i]);
    }
}
void executeGrassWide(const kzgs::PacketView *views, uint32_t count)
{
    static_assert(tf_gpu::MaxMergedGrassPackets <= kzgs::MaxPacketGroup);
    if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12)
    {
        for (unsigned i = 0; i < count; ++i)
            execute(views[i].data, views[i].bytes);
        return;
    }
    if (count <= 8)
    {
        executeGroup(views, count);
        return;
    }
    if (!live)
        live = std::make_unique<Live>(*GSDevice12::GetInstance());
    std::array<tf_gpu::GrassPacketView, tf_gpu::MaxMergedGrassPackets> packets;
    bool valid = count <= packets.size();
    for (unsigned i = 0; valid && i < count; ++i)
    {
        if (views[i].bytes < sizeof(Payload))
        {
            valid = false;
            break;
        }
        const auto &payload = *reinterpret_cast<const Payload *>(views[i].data);
        valid = payload.path == 1 && payload.bytes == views[i].bytes - sizeof(Payload) &&
                payload.geometry.kind == gsnative::GeometryKind::GrassSprites;
        packets[i] = {&payload.geometry, views[i].data + sizeof(Payload), payload.bytes};
    }
    if (valid && tf_gpu::drawGrassGroup(live->pool, packets.data(), count, live->grass))
        return;
    // Rejected groups have consumed no state. Split in order until smaller
    // admissible runs or the established eight-packet fallback are reached.
    const unsigned first = count / 2;
    executeGrassWide(views, first);
    executeGrassWide(views + first, count - first);
}
} // namespace

extern "C" int tf_gs_native_dual_gif(uint32_t path, const uint8_t *data, uint32_t bytes,
                                     const gsnative::DualPacket *geometry)
{
    if (path != 1 || !data || !geometry ||
        (!tf_gpu::dualPacketFraming(*geometry, data, bytes) && !tf_gpu::grassPacketFraming(*geometry, data, bytes) &&
         !tf_gpu::reflectionPacketFraming(*geometry, data, bytes)))
        return 0;
    alignas(16) std::array<uint8_t, sizeof(Payload) + 4 * (4 + 3 * gsnative::ReflectionMaxVertices) * 16> storage;
    const Payload payload{*geometry, path, bytes, 0};
    std::memcpy(storage.data(), &payload, sizeof(payload));
    std::memcpy(storage.data() + sizeof(payload), data, bytes);
    // Four ordered reflection sections are not part of the dual/grass merger.
    // An ordinary owned command also forms an explicit group boundary.
    if (geometry->kind == gsnative::GeometryKind::Reflection)
        return kzgs::QueuePacketCommand(execute, storage.data(), uint32_t(sizeof(payload) + bytes)) ? 1 : 0;
    static const bool grouped = [] {
        const char *p = std::getenv("PS2X_TF_GPU_GROUPS");
        return p && !std::strcmp(p, "8");
    }();
    if (grouped)
    {
        static const bool wideGrass = [] {
            const char *size = std::getenv("PS2X_TF_GPU_GRASS_GROUPS");
            const char *merge = std::getenv("PS2X_TF_GPU_GRASS_MERGE");
            return size && !std::strcmp(size, "64") && merge && !std::strcmp(merge, "1");
        }();
        if (wideGrass && geometry->kind == gsnative::GeometryKind::GrassSprites)
            return kzgs::QueuePacketGroupCommand(executeGrassWide, storage.data(), uint32_t(sizeof(payload) + bytes),
                                                 tf_gpu::MaxMergedGrassPackets)
                       ? 1
                       : 0;
        return kzgs::QueuePacketGroupCommand(executeGroup, storage.data(), uint32_t(sizeof(payload) + bytes)) ? 1 : 0;
    }
    return kzgs::QueuePacketCommand(execute, storage.data(), uint32_t(sizeof(payload) + bytes)) ? 1 : 0;
}
void tfGpuLiveDrawReset()
{
    kzgs::RunOnGSThread([] {
        if (live)
            live->report();
        live.reset();
    });
}
