// SPDX-License-Identifier: GPL-3.0-or-later
// Private enhanced captures through the real GS texture/depth/blend frontend.
// The comparison uses GIF vertices; native preparation uses only mesh bounds.
#include "tf_hwgs.h"
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_dual_frontend.h"
#include "tf_gpu_live_dual.h"
#include "tf_gpu_merge_guards.h"
#include "runtime/gs/gs_native_dual_math.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
uint32_t word(const void *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
struct QueueCheck
{
    std::atomic<bool> ready{false};
    uint32_t next = 0;
    bool valid = true;
    std::array<unsigned, kzgs::MaxPacketGroup + 1> sizes{};
};
struct QueueHeader
{
    QueueCheck *check;
    uint32_t serial, bytes;
};
void checkQueuedPacket(const uint8_t *data, uint32_t bytes)
{
    QueueHeader header;
    std::memcpy(&header, data, sizeof(header));
    auto &check = *header.check;
    check.valid &= header.serial == check.next++ && header.bytes == bytes &&
                   word(data + bytes - 4) == (header.serial ^ 0xa53f9c17u);
}
void checkQueuedGroup(const kzgs::PacketView *views, uint32_t count)
{
    QueueHeader first;
    std::memcpy(&first, views[0].data, sizeof(first));
    ++first.check->sizes[count];
    for (unsigned i = 0; i < count; ++i)
        checkQueuedPacket(views[i].data, views[i].bytes);
}
void checkOtherGroup(const kzgs::PacketView *views, uint32_t count)
{
    checkQueuedGroup(views, count);
}
bool checkPacketQueue()
{
    QueueCheck check;
    std::vector<uint8_t> storage(512 * 1024);
    QueueCheck *address = &check;
    bool queued = kzgs::QueuePacketCommand(
        [](const uint8_t *data, uint32_t) {
            QueueCheck *state;
            std::memcpy(&state, data, sizeof(state));
            state->ready.wait(false, std::memory_order_acquire);
        },
        &address, sizeof(address));
    const auto push = [&](unsigned serial, uint32_t bytes, unsigned kind, unsigned maximum = 8) {
        const QueueHeader header{&check, serial, bytes};
        std::memcpy(storage.data(), &header, sizeof(header));
        const uint32_t guard = serial ^ 0xa53f9c17u;
        std::memcpy(storage.data() + bytes - 4, &guard, 4);
        if (!kind)
            return kzgs::QueuePacketCommand(checkQueuedPacket, storage.data(), bytes);
        return kzgs::QueuePacketGroupCommand(kind == 1 ? checkQueuedGroup : checkOtherGroup, storage.data(), bytes,
                                             maximum);
    };
    // Publish everything before releasing the consumer: 8,8,4 / ordinary /
    // 3 / different callback 2 / original callback 1. Boundaries are observable.
    for (unsigned i = 0; i < 27; ++i)
        queued &= push(i, 64, i == 20 ? 0 : (i == 24 || i == 25 ? 2 : 1));
    check.ready.store(true, std::memory_order_release);
    check.ready.notify_all();
    kzgs::RunOnGSThread([] {});
    const bool first = check.valid && check.next == 27 && check.sizes[8] == 2 && check.sizes[4] == 1 &&
                       check.sizes[3] == 1 && check.sizes[2] == 1 && check.sizes[1] == 1;
    // More than one entire ring of owned payloads exercises wrap and reuse.
    for (unsigned i = 27; i < 95; ++i)
        queued &= push(i, uint32_t(storage.size()), i % 7 ? 1 : 0);
    kzgs::RunOnGSThread([] {});
    const bool wrapped = check.valid && check.next == 95;
    check.ready.store(false, std::memory_order_relaxed);
    check.sizes.fill(0);
    queued &= kzgs::QueuePacketCommand(
        [](const uint8_t *data, uint32_t) {
            QueueCheck *state;
            std::memcpy(&state, data, sizeof(state));
            state->ready.wait(false, std::memory_order_acquire);
        },
        &address, sizeof(address));
    // A wider group, then the same callback with a smaller limit, then back to
    // wide, ordinary and foreign-callback boundaries. None may be crossed.
    for (unsigned i = 95; i < 165; ++i)
        queued &= push(i, 64, 1, 64);
    for (unsigned i = 165; i < 175; ++i)
        queued &= push(i, 64, 1, 8);
    for (unsigned i = 175; i < 178; ++i)
        queued &= push(i, 64, 1, 64);
    queued &= push(178, 64, 0);
    queued &= push(179, 64, 2, 64) && push(180, 64, 2, 64);
    queued &= !push(181, 64, 1, 0) && !push(181, 64, 1, kzgs::MaxPacketGroup + 1);
    check.ready.store(true, std::memory_order_release);
    check.ready.notify_all();
    kzgs::RunOnGSThread([] {});
    const bool wide = check.valid && check.next == 181 && check.sizes[64] == 1 && check.sizes[6] == 1 &&
                      check.sizes[8] == 1 && check.sizes[3] == 1 && check.sizes[2] == 2;
    const bool passed = queued && first && wrapped && wide;
    std::fprintf(stderr, "[gpu:packet-groups] packets=%u order_wrap_boundaries=%s\n", check.next,
                 passed ? "PASS" : "FAIL");
    return passed;
}
struct alignas(16) Qword
{
    uint64_t low, high;
};
void transfer(std::vector<Qword> &packet)
{
    // PACKED A+D, EOP, one register. All payloads are generated here.
    packet[0] = {uint64_t(packet.size() - 1) | (1ull << 15) | (1ull << 60), GIF_REG_A_D};
    g_gs_renderer->Transfer<2>(reinterpret_cast<const uint8_t *>(packet.data()), uint32_t(packet.size()));
}
template <typename T> void reg(std::vector<Qword> &packet, unsigned address, const T &value)
{
    static_assert(sizeof(T) >= 8);
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    packet.push_back({bits, address});
}
void cpuDraw(const GSVertex *vertices, const std::vector<uint16_t> &indices)
{
    std::vector<Qword> packet(1);
    for (const auto index : indices)
    {
        const auto &v = vertices[index];
        reg(packet, 0x01, v.RGBAQ);
        reg(packet, 0x02, v.ST);
        packet.push_back({uint64_t(v.FOG) << 56, 0x0a});
        reg(packet, 0x05, v.XYZ);
    }
    transfer(packet);
    g_gs_renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
}
struct Submission
{
    tf_gpu::DualGeometry *pool;
    const tf_gpu::DualGeometry::Batch *batch;
    const GSVertex *reference;
    uint32_t start, count;
    uint64_t *stats;
    bool forceFallback = false;
};
bool submit(void *context, GSHWDrawConfig &config)
{
    auto &s = *static_cast<Submission *>(context);
    ++s.stats[1];
    s.stats[3] += config.tex != nullptr;
    s.stats[4] += config.ds != nullptr;
    s.stats[5] += config.blend.enable || config.ps.IsSWBlending();
    s.stats[6] += config.ps.automatic_lod || config.ps.manual_lod;
    s.stats[13] += config.sampler.UseMipmapFiltering();
    if (s.forceFallback)
        return false;
    return s.pool->draw(*s.batch, config, s.start, s.count);
}
void materialize(void *context, GSVertex *vertices, uint32_t count)
{
    auto &s = *static_cast<Submission *>(context);
    ++s.stats[2];
    std::memcpy(vertices, s.reference, size_t(count) * sizeof(GSVertex));
}

void packetScene(const void *inputs, const uint8_t *constants, const void *packed, unsigned vertices, unsigned count,
                 unsigned side, unsigned mode, GIFRegPRIM prim, tf_gpu::DualGeometry &pool,
                 tf_gpu::DualMergeStats &stats, std::vector<GSVertex> &latches, std::vector<bool> &latchEnds,
                 uint64_t *counters, tf_gpu::DualMeshCache *cache)
{
    auto *renderer = GSRendererHW::GetInstance();
    struct alignas(16) RawPacket
    {
        std::array<uint8_t, (8 + 28 * 6) * 16> data{};
    };
    std::vector<RawPacket> raw(count);
    std::vector<gsnative::DualPacket> sources(count);
    std::vector<tf_gpu::DualPacketView> views(count);
    std::vector<tf_gpu::PreparedDual> prepared(count);
    prim.PRIM = GS_TRIANGLESTRIP;
    for (unsigned packet = 0; packet < count; ++packet)
    {
        const unsigned first = word(constants + packet * 144 + 140);
        const unsigned last = packet + 1 == count ? vertices : word(constants + (packet + 1) * 144 + 140);
        require(last > first && last - first >= 3 && last - first <= 28 && last <= vertices,
                "Invalid packet source range");
        auto &source = sources[packet];
        source.version = 2;
        source.vertices = last - first;
        std::memcpy(source.constants.data(), constants + packet * 144, 144);
        std::memset(source.constants.data() + 140, 0, 4);
        std::memcpy(source.inputs.data(), static_cast<const uint8_t *>(inputs) + first * 32, source.vertices * 32);
        for (unsigned i = 0; i < source.vertices; ++i)
            std::memset(source.inputs.data() + i * 32 + 12, 0, 4);
        for (unsigned field = 0; field < 2; ++field)
        {
            auto *qwords = reinterpret_cast<Qword *>(raw[packet].data.data()) + field * (4 + source.vertices * 3);
            qwords[0] = {2ull | (1ull << 60), GIF_REG_A_D};
            GIFRegSCISSOR scissor{};
            scissor.SCAX1 = 511;
            scissor.SCAY0 = field ? 224 : 0;
            scissor.SCAY1 = field ? 447 : 223;
            GIFRegXYOFFSET offset{};
            offset.OFX = 28671;
            offset.OFY = field ? 25591 : 29183;
            qwords[1] = {scissor.U64, uint64_t(0x40 + prim.CTXT)};
            qwords[2] = {offset.U64, uint64_t(0x18 + prim.CTXT)};
            qwords[3] = {source.vertices | (uint64_t(field) << 15) | (1ull << 46) | (prim.U64 << 47) | (3ull << 60),
                         0x412};
            unsigned firstZ = 0;
            bool found = false;
            for (unsigned i = 0; i < source.vertices; ++i)
            {
                const auto *p = static_cast<const uint8_t *>(packed) + ((first + i) * 2 + field) * 48;
                std::memcpy(qwords + 4 + i * 3, p, 48);
                const uint32_t mask = word(source.constants.data() + 132 + (i / 16) * 4);
                if (i < 2 || ((mask << (i % 16)) & 32768))
                    continue;
                for (unsigned corner = i - 2; corner <= i; ++corner)
                {
                    const auto *v = static_cast<const uint8_t *>(packed) + ((first + corner) * 2 + field) * 48;
                    const unsigned z = (word(v + 40) >> 4) & 0xffffff;
                    if (found && z != firstZ)
                        source.varyingDepth |= 1u << field;
                    firstZ = z;
                    found = true;
                }
            }
            if (!side)
                for (unsigned i = 0; i + 2 < source.vertices; ++i)
                {
                    auto *p = reinterpret_cast<uint8_t *>(qwords + 4 + i * 3);
                    std::memset(p, 0xcd, 12);
                    std::memset(p + 32, 0xcd, 12);
                }
        }
        views[packet] = {&source, raw[packet].data.data(), (8 + source.vertices * 6) * 16};
        require(tf_gpu::dualPacketFraming(source, views[packet].packet, views[packet].bytes),
                "Invalid constructed mesh packet");
        if (!side)
        {
            prepared[packet].mesh = pool.uploadMesh(source.inputs.data(), source.vertices, 1);
            require(bool(prepared[packet].mesh) &&
                        pool.bounds(prepared[packet].mesh, source.constants.data(), 1, 0, 1, prepared[packet].bounds),
                    "Cannot prepare packet bounds");
        }
    }
    if (!side && !mode)
    {
        const auto decline = [&](unsigned n) {
            const auto environment = renderer->m_env;
            const auto latch = renderer->GetNativeVertexLatch();
            const auto dispatches = pool.stats().dispatches;
            require(!tf_gpu::drawDualGroup(pool, views.data(), prepared.data(), n, stats, false, cache),
                    "Unsafe dual merge accepted");
            require(dispatches == pool.stats().dispatches &&
                        !std::memcmp(&environment, &renderer->m_env, sizeof(environment)) &&
                        !std::memcmp(&latch, &renderer->GetNativeVertexLatch(), sizeof(latch)),
                    "Declined merge changed GS state");
            ++counters[17];
        };
        decline(0);
        decline(cache ? tf_gpu::MaxDualMeshPackets + 1 : 9);
        const auto saved0 = raw[0], saved1 = raw[1];
        raw[1].data[16] ^= 1;
        decline(2);
        raw[1] = saved1;
        for (unsigned i = 0; i < 2; ++i)
            std::memcpy(raw[i].data.data() + (4 + sources[i].vertices * 3) * 16 + 16, raw[i].data.data() + 16, 8);
        decline(2);
        raw[0] = saved0;
        raw[1] = saved1;
        auto &context = renderer->m_env.CTXT[prim.CTXT];
        const auto saved = context;
        context.TEX0.TBP0 = context.FRAME.Block();
        decline(2);
        context = saved;
        context.TEX0.TBP0 = context.ZBUF.Block();
        decline(2);
        context = saved;
        context.TEX0.PSM = PSMT8;
        context.TEX0.CBP = context.FRAME.Block();
        decline(2);
        context = saved;
        context.TEX1.MXL = 1;
        context.MIPTBP1.TBP1 = context.FRAME.Block();
        context.MIPTBP1.TBW1 = 1;
        decline(2);
        context = saved;
        sources[0].vertices = 2;
        decline(2);
        sources[0].vertices = (views[0].bytes / 16 - 8) / 6;
        if (cache)
        {
            // Cache identity includes every source byte and restart mask, but
            // excludes camera transforms. Deliberately change both kinds of
            // immutable data, then restore them and require the original hit.
            require(tf_gpu::dualMeshTransformsMatch(views[0], views[1]), "Cache test requires a shared transform");
            auto *original = cache->prepare(views.data(), 2, stats);
            require(original != nullptr, "Cannot prepare cache identity fixture");
            const auto originalIndices = original->indices[0].size();
            const auto uploads = stats.cacheUploads;
            uint32_t savedPosition;
            std::memcpy(&savedPosition, sources[0].inputs.data(), 4);
            const uint32_t changed = savedPosition ^ 1u;
            std::memcpy(sources[0].inputs.data(), &changed, 4);
            require(cache->prepare(views.data(), 2, stats) && stats.cacheUploads == uploads + 1,
                    "Changed mesh source reused stale GPU vertices");
            std::memcpy(sources[0].inputs.data(), &savedPosition, 4);
            sources[0].constants[133] ^= 0x20; // toggle the first triangle's ADC bit
            auto *cut = cache->prepare(views.data(), 2, stats);
            require(cut && stats.cacheUploads == uploads + 2 && cut->indices[0].size() != originalIndices,
                    "Changed strip restart reused stale topology");
            sources[0].constants[133] ^= 0x20;
            const auto hits = stats.cacheHits;
            require(cache->prepare(views.data(), 2, stats) == original && stats.cacheHits == hits + 1,
                    "Restored source did not reuse its resident mesh");
            const auto savedMatrix = sources[1].constants[0];
            sources[1].constants[0] ^= 1;
            require(cache->prepare(views.data(), 2, stats) == original && stats.cacheUploads == uploads + 2,
                    "Camera-only change replaced immutable source geometry");
            sources[1].constants[0] = savedMatrix;
            counters[17] += 4;
        }
    }
    const auto ordinary = [&](unsigned i) {
        if (!side)
            gsnative::materializeDualPacket(sources[i], raw[i].data.data(), views[i].bytes);
        kzgs::ExecuteGIFOnGSThread(1, raw[i].data.data(), views[i].bytes);
    };
    if (!side)
    {
        std::fill(latchEnds.begin(), latchEnds.end(), false);
        for (unsigned i = 0, group = 0; i < count; ++group)
        {
            constexpr unsigned sizes[] = {2, 3, 4, 8};
            unsigned n = std::min(sizes[group % 4], count - i);
            if (cache)
            {
                constexpr unsigned wideSizes[] = {2, 3, 4, 8, 16, 32, tf_gpu::MaxDualMeshPackets};
                n = std::min(wideSizes[group % 7], count - i);
            }
            if (n < 2 || !tf_gpu::drawDualGroup(pool, views.data() + i, prepared.data() + i, n, stats, mode == 3, cache))
                for (unsigned j = i; j < i + n; ++j)
                    ordinary(j);
            latches[i + n - 1] = renderer->GetNativeVertexLatch();
            latchEnds[i + n - 1] = true;
            require(renderer->m_path[0].nloop == 0, "Merged dual draw left unfinished GIF");
            i += n;
        }
    }
    else
        for (unsigned i = 0; i < count; ++i)
        {
            ordinary(i);
            if (latchEnds[i])
            {
                require(!std::memcmp(&latches[i], &renderer->GetNativeVertexLatch(), sizeof(GSVertex)),
                        "Merged dual latch changed");
                ++counters[16];
            }
        }
    renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
}
} // namespace

static int frontendProbe(const void *inputs, uint32_t count, const void *constants, uint32_t batches,
                         const uint32_t *groupEnds, uint32_t groups, const void *expected, uint64_t *stats, char *error,
                         uint32_t capacity, const void *packed = nullptr)
{
    if (!inputs || !constants || !expected || !stats || !groupEnds || !groups || groups > batches || count < 3 ||
        count > 32768 || !batches || batches > 1024)
        return 0;
    bool passed = false;
    const char *groupOption = std::getenv("PS2X_TF_GPU_GROUPS");
    const bool grouped = groupOption && !std::strcmp(groupOption, "8");
    if (grouped && !checkPacketQueue())
    {
        if (error && capacity)
            std::snprintf(error, capacity, "Ordered packet group diagnostic failed");
        return 0;
    }
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Frontend probe requires D3D12");
            auto *renderer = GSRendererHW::GetInstance();
            auto *device = GSDevice12::GetInstance();
            const auto *transforms = static_cast<const uint8_t *>(constants);
            std::vector<GSVertex> reference(count * 2);
            std::memcpy(reference.data(), expected, size_t(count) * 64);
            tf_gpu::DualGeometry pool(*device);
            tf_gpu::DualMergeStats merged;
            const char *meshOption = std::getenv("PS2X_TF_GPU_DUAL_BATCHES");
            std::unique_ptr<tf_gpu::DualMeshCache> meshCache;
            if (meshOption && !std::strcmp(meshOption, "1")) meshCache = std::make_unique<tf_gpu::DualMeshCache>(pool);
            std::vector<GSVertex> packetLatches(batches);
            std::vector<bool> packetLatchEnds(batches);
            if (packed)
                require(batches >= 2 && tf_gpu::probeMergeRanges(stats[18]), "Invalid mesh packet fixture/ranges");
            auto mesh = pool.uploadMesh(inputs, count, batches);
            require(bool(mesh), "Cannot upload frontend mesh");
            auto output = pool.prepare(mesh, constants, batches);
            require(bool(output), "Cannot dispatch frontend geometry");
            std::vector<std::array<tf_gpu::DualDrawBounds, 2>> bounds(groups);
            unsigned previous = 0;
            for (unsigned group = 0; group < groups; ++group)
            {
                require(groupEnds[group] > previous && groupEnds[group] <= batches, "Invalid frontend mesh groups");
                require(pool.bounds(mesh, constants, batches, previous, groupEnds[group] - previous, bounds[group]),
                        "Cannot prepare frontend mesh bounds");
                previous = groupEnds[group];
            }
            require(previous == batches, "Incomplete frontend mesh groups");
            const auto chunkStart = [&](unsigned chunk) {
                return chunk == batches ? count : word(transforms + chunk * 144 + 140);
            };
            std::vector<tf_gpu::DualGeometry::Batch> groupedOutputs(groups);
            if (grouped && !packed)
            {
                require(!pool.prepareGroup(nullptr, constants, 1) && !pool.prepareGroup(&mesh, nullptr, 1) &&
                            !pool.prepareGroup(&mesh, constants, 0) && !pool.prepareGroup(&mesh, constants, 9),
                        "Invalid grouped input was accepted");
                for (unsigned group = 0; group < groups; ++group)
                {
                    const unsigned first = group ? groupEnds[group - 1] : 0, last = groupEnds[group];
                    if (last - first > 8)
                        continue;
                    std::array<tf_gpu::DualGeometry::Mesh, 8> sources;
                    std::array<uint8_t, 8 * 144> records{};
                    for (unsigned chunk = first; chunk < last; ++chunk)
                    {
                        const unsigned start = chunkStart(chunk), end = chunkStart(chunk + 1), index = chunk - first;
                        const auto *data = static_cast<const uint8_t *>(inputs);
                        std::vector<uint8_t> vertices(data + start * 32, data + end * 32);
                        for (unsigned i = 0; i < end - start; ++i)
                            std::memset(vertices.data() + i * 32 + 12, 0, 4);
                        sources[index] = pool.uploadMesh(vertices.data(), end - start, 1);
                        require(bool(sources[index]), "Cannot upload independent grouped mesh");
                        std::memcpy(records.data() + index * 144, transforms + chunk * 144, 144);
                        std::memset(records.data() + index * 144 + 140, 0, 4);
                    }
                    if (!group)
                    {
                        auto bad = records;
                        bad[140] = 1;
                        const auto dispatches = pool.stats().dispatches;
                        tf_gpu::DualGeometry foreign(*device);
                        require(!pool.prepareGroup(sources.data(), bad.data(), last - first) &&
                                    !foreign.prepareGroup(sources.data(), records.data(), last - first) &&
                                    pool.stats().dispatches == dispatches,
                                "Invalid grouped layout/ownership dispatched work");
                    }
                    groupedOutputs[group] = pool.prepareGroup(sources.data(), records.data(), last - first);
                    require(bool(groupedOutputs[group]), "Cannot dispatch independent grouped geometry");
                }
                require(pool.stats().groupedDispatches > 0 && pool.stats().groupedMeshes > 8,
                        "Grouped diagnostic did not exercise independent source buffers");
            }
            // Independent render pairs exercise texture lookup, depth, alpha
            // blending/testing, mipmaps and deliberate CPU fallback. Packet
            // merging also separates opaque and blended mipmap cases.
            for (unsigned mode = 0; mode < (packed ? 5u : 4u); ++mode)
            {
                const uint64_t fallbackBefore = stats[2];
                const auto mergedFallbackBefore = merged.materializations;
                const auto mergedGpuBefore = merged.gpuDraws;
                renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
                renderer->ReadbackTextureCache();
                GIFRegBITBLTBUF blit{};
                blit.DBW = 8;
                blit.DPSM = PSMCT32;
                std::memset(renderer->m_mem.m_vm8, 0, 4 * 1024 * 1024);
                renderer->InvalidateVideoMem(blit, GSVector4i(0, 0, 512, 2048));
                constexpr unsigned textureBase = 15360;
                for (unsigned lod = 0; lod < 2; ++lod)
                {
                    const unsigned size = 64 >> lod, base = textureBase + lod * 64;
                    for (unsigned y = 0; y < size; ++y)
                        for (unsigned x = 0; x < size; ++x)
                        {
                            const uint32_t checker = ((x / 4) ^ (y / 4)) & 1;
                            const uint32_t color = checker ? 0x807040e0u : 0x4040e070u;
                            renderer->m_mem.WritePixel32(x, y, lod ? color ^ 0x00202020 : color, base, 1);
                        }
                }
                for (unsigned side = 0; side < 2; ++side)
                {
                    const unsigned framebuffer = side * 112, depth = 224 + side * 112;
                    // Establish identical full-size cached targets before the
                    // two preparations compute different conservative boxes.
                    {
                        GIFRegPRIM clearPrim{};
                        clearPrim.PRIM = GS_SPRITE;
                        GIFRegFRAME clearFrame{};
                        clearFrame.FBP = framebuffer;
                        clearFrame.FBW = 8;
                        GIFRegZBUF clearZ{};
                        clearZ.ZBP = depth;
                        clearZ.PSM = PSMZ32;
                        GIFRegTEST clearTest{};
                        clearTest.ZTE = 1;
                        clearTest.ZTST = ZTST_ALWAYS;
                        GIFRegSCISSOR clearScissor{};
                        clearScissor.SCAX1 = 511;
                        clearScissor.SCAY1 = 447;
                        std::vector<Qword> clear(1);
                        clear.push_back({1, 0x1a});
                        reg(clear, 0x00, clearPrim);
                        reg(clear, 0x4c, clearFrame);
                        reg(clear, 0x4e, clearZ);
                        reg(clear, 0x47, clearTest);
                        clear.push_back({0, 0x18});
                        reg(clear, 0x40, clearScissor);
                        clear.push_back({0x20102010, 0x01});
                        clear.push_back({0, 0x05});
                        clear.push_back({uint64_t(512 * 16) | (uint64_t(448 * 16) << 16), 0x05});
                        transfer(clear);
                        renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
                    }
                    for (unsigned field = 0; field < 2; ++field)
                    {
                        if (packed && field)
                            break;
                        GIFRegPRIM prim{};
                        prim.PRIM = GS_TRIANGLELIST;
                        prim.IIP = prim.TME = 1;
                        prim.ABE = mode == 1 || (packed ? mode == 4 : mode == 2);
                        GIFRegFRAME frame{};
                        frame.FBP = framebuffer;
                        frame.FBW = 8;
                        GIFRegZBUF zbuf{};
                        zbuf.ZBP = depth;
                        zbuf.PSM = PSMZ32;
                        GIFRegTEST test{};
                        test.ZTE = 1;
                        test.ZTST = ZTST_GEQUAL;
                        test.ATE = mode == 1;
                        test.ATST = ATST_GEQUAL;
                        test.AREF = 32;
                        GIFRegALPHA alpha{};
                        alpha.A = 0;
                        alpha.B = 1;
                        alpha.C = 0;
                        alpha.D = 1;
                        GIFRegTEX0 tex{};
                        tex.TBP0 = textureBase;
                        tex.TBW = 1;
                        tex.TW = tex.TH = 6;
                        tex.TCC = 1;
                        GIFRegTEX1 tex1{};
                        tex1.MMAG = 1;
                        const bool mipmap = mode == 2 || mode == 4;
                        tex1.MMIN = mipmap ? 5 : 1;
                        tex1.MXL = mipmap ? 1 : 0;
                        GIFRegMIPTBP1 mip{};
                        mip.TBP1 = textureBase + 64;
                        mip.TBW1 = 1;
                        GIFRegXYOFFSET xy{};
                        xy.OFX = 28671;
                        xy.OFY = field ? 25591 : 29183;
                        GIFRegSCISSOR scissor{};
                        scissor.SCAX1 = 511;
                        scissor.SCAY0 = field ? 224 : 0;
                        scissor.SCAY1 = field ? 447 : 223;
                        std::vector<Qword> state(1);
                        state.push_back({1, 0x1a}); // PRMODECONT
                        state.push_back({1, 0x46}); // COLCLAMP
                        reg(state, 0x00, prim);
                        reg(state, 0x4c, frame);
                        reg(state, 0x4e, zbuf);
                        reg(state, 0x47, test);
                        reg(state, 0x42, alpha);
                        reg(state, 0x06, tex);
                        reg(state, 0x14, tex1);
                        reg(state, 0x34, mip);
                        reg(state, 0x18, xy);
                        reg(state, 0x40, scissor);
                        transfer(state);
                        if (packed)
                        {
                            packetScene(inputs, transforms, packed, count, batches, side, mode, prim, pool, merged,
                                        packetLatches, packetLatchEnds, stats, meshCache.get());
                            continue;
                        }
                        for (unsigned group = 0; group < groups; ++group)
                        {
                            const unsigned firstChunk = group ? groupEnds[group - 1] : 0, lastChunk = groupEnds[group];
                            const unsigned start = chunkStart(firstChunk), end = chunkStart(lastChunk);
                            std::vector<uint16_t> indices;
                            for (unsigned chunk = firstChunk; chunk < lastChunk; ++chunk)
                            {
                                const unsigned first = chunkStart(chunk), last = chunkStart(chunk + 1);
                                for (unsigned i = 2; i < last - first; ++i)
                                {
                                    const uint32_t mask = word(transforms + chunk * 144 + 132 + (i / 16) * 4);
                                    if ((mask << (i % 16)) & 32768)
                                        continue;
                                    for (unsigned corner = i - 2; corner <= i; ++corner)
                                        indices.push_back(uint16_t(2 * (first - start + corner) + field));
                                }
                            }
                            if (indices.empty())
                                continue;
                            const GSVertex *vertices = reference.data() + start * 2;
                            // Diagnostic-only independent eligibility witness. A
                            // live caller must prove this from its source metadata.
                            const float firstZ = float(vertices[indices.front()].XYZ.Z);
                            const bool varyingDepth = std::any_of(indices.begin(), indices.end(), [&](uint16_t i) {
                                return float(vertices[i].XYZ.Z) != firstZ;
                            });
                            GSVertexTrace::Vertex minimum{}, maximum{};
                            const auto &selected = groupedOutputs[group] ? groupedOutputs[group] : output;
                            Submission submission{&pool,       &selected, vertices, groupedOutputs[group] ? 0u : start,
                                                  end - start, stats,     mode == 3};
                            if (!side && varyingDepth &&
                                tf_gpu::prepareDualTrace(bounds[group][field], *renderer->m_context, *renderer->PRIM,
                                                         minimum, maximum))
                            {
                                if (!mode && !field && !group)
                                {
                                    auto invalid = minimum;
                                    invalid.t.x = std::numeric_limits<float>::quiet_NaN();
                                    require(!renderer->DrawNativeTriangles(invalid, maximum, indices.data(),
                                                                           uint32_t(indices.size()), (end - start) * 2,
                                                                           true, &submission, submit, materialize),
                                            "Nonfinite native bounds were accepted");
                                    require(!renderer->DrawNativeTriangles(minimum, maximum, indices.data(),
                                                                           uint32_t(indices.size()), (end - start) * 2,
                                                                           false, &submission, submit, materialize),
                                            "Missing nonconstant-depth proof was accepted");
                                    auto badIndices = indices;
                                    badIndices[0] = uint16_t((end - start) * 2);
                                    require(!renderer->DrawNativeTriangles(
                                                minimum, maximum, badIndices.data(), uint32_t(badIndices.size()),
                                                (end - start) * 2, true, &submission, submit, materialize),
                                            "Out-of-range native indices were accepted");
                                    require(stats[1] == 0 && stats[2] == 0, "Declined draws invoked native callbacks");
                                }
                                require(renderer->DrawNativeTriangles(minimum, maximum, indices.data(),
                                                                      uint32_t(indices.size()), (end - start) * 2, true,
                                                                      &submission, submit, materialize),
                                        "Renderer declined qualified diagnostic triangles");
                                ++stats[0];
                            }
                            else
                                cpuDraw(vertices, indices);
                        }
                    }
                }
                // Compare the complete upscaled images before reducing them to
                // GS VRAM. Reduced readback can hide subpixel disagreements.
                std::array<std::vector<uint8_t>, 2> images;
                unsigned imageWidth = 0, imageHeight = 0;
                for (unsigned side = 0; side < 2; ++side)
                {
                    const unsigned base = side * 112 * 32;
                    auto *target = g_texture_cache->GetExactTarget(base, 8, GSTextureCache::RenderTarget, base + 1);
                    require(target && target->m_texture && target->m_TEX0.TBP0 == base,
                            "Missing exact frontend render target");
                    auto *texture = target->m_texture;
                    const unsigned width = texture->GetWidth(), height = texture->GetHeight();
                    if (side)
                        require(width == imageWidth && height == imageHeight, "Frontend target dimensions differ");
                    imageWidth = width;
                    imageHeight = height;
                    require(width >= 2048 && height >= 1792, "Frontend targets are below the requested internal size");
                    const GSVector4i area(0, 0, width, height);
                    auto download = device->CreateDownloadTexture(width, height, GSTexture::Format::Color);
                    require(bool(download), "Cannot create frontend image download");
                    download->CopyFromTexture(area, texture, area, 0);
                    download->Flush();
                    images[side].resize(size_t(width) * height * 4);
                    require(download->ReadTexels(area, images[side].data(), width * 4),
                            "Cannot download frontend image");
                }
                uint64_t fullDifferences = 0;
                for (size_t pixel = 0; pixel < images[0].size(); pixel += 4)
                    fullDifferences += std::memcmp(images[0].data() + pixel, images[1].data() + pixel, 4) != 0;
                stats[10] += fullDifferences;
                stats[11] += uint64_t(imageWidth) * imageHeight;
                std::fprintf(stderr, "[gpu:frontend] mode=%u image=%ux%u full_differences=%llu\n", mode, imageWidth,
                             imageHeight, fullDifferences);
                renderer->ReadbackTextureCache();
                uint64_t painted = 0, differences = 0;
                for (unsigned y = 0; y < 448; ++y)
                    for (unsigned x = 0; x < 512; ++x)
                    {
                        const auto actual = renderer->m_mem.ReadPixel32(x, y, 0, 8);
                        const auto expectedPixel = renderer->m_mem.ReadPixel32(x, y, 112 * 32, 8);
                        painted += actual != 0x20102010;
                        if (actual != expectedPixel && differences < 8)
                            std::fprintf(stderr, "[gpu:frontend] mode=%u pixel=%u,%u native=%08x gif=%08x\n", mode, x,
                                         y, actual, expectedPixel);
                        differences += actual != expectedPixel;
                    }
                stats[7] += painted;
                stats[8] += differences;
                ++stats[9];
                require(painted > 1000, "Frontend comparison did not paint enough pixels");
                require(mode == 3 || stats[2] == fallbackBefore,
                        "A GPU frontend mode unexpectedly materialized CPU vertices");
                require(!packed || mode == 3 || merged.materializations == mergedFallbackBefore,
                        "A GPU merged mode unexpectedly materialized vertices");
                if (packed)
                    require(mode == 3 ? merged.materializations > mergedFallbackBefore
                                      : merged.gpuDraws > mergedGpuBefore,
                            "Merged mode did not exercise its GPU/fallback path");
                std::fprintf(stderr,
                             "[gpu:frontend] mode=%u painted=%llu differences=%llu submits=%llu fallback=%llu\n", mode,
                             painted, differences, stats[1], stats[2]);
            }
            device->ExecuteCommandList(true);
            stats[12] = pool.stats().draws;
            stats[14] = pool.stats().groupedDispatches;
            stats[15] = pool.stats().groupedMeshes;
            require(stats[8] == 0 && stats[10] == 0, "Native frontend pixels differ from GIF reference draws");
            if (packed)
            {
                stats[19] = merged.groups;
                stats[20] = merged.packets;
                stats[21] = merged.gpuDraws;
                stats[22] = merged.materializations;
                stats[23] = merged.ordinaryFields;
                stats[24] = merged.stateDeclined;
                stats[25] = merged.feedbackDeclined;
                stats[26] = merged.boundsDeclined;
                stats[27] = merged.outputDeclined;
                if (meshCache)
                {
                    require(merged.nativeBatches > 100 && merged.cacheHits > 100 && merged.largestBatch > 8 &&
                                merged.cacheUploads > 0,
                            "Resident mesh test did not exercise wide groups and source reuse");
                    std::fprintf(stderr,
                                 "[gpu-mesh:qualification] batches=%llu packets=%llu cache_hits=%llu cache_uploads=%llu largest=%llu\n",
                                 merged.nativeBatches, merged.nativePackets, merged.cacheHits, merged.cacheUploads,
                                 merged.largestBatch);
                }
                require(merged.groups > 100 && merged.gpuDraws > 100 && merged.materializations > 0 &&
                            stats[16] > 100 && stats[17] >= 9,
                        "Merged dual diagnostic did not exercise required paths");
            }
            else
                require(stats[0] > 100 && stats[1] > 100 && stats[2] > 0 && stats[3] > 100 && stats[4] > 100 &&
                            stats[5] > 0 && stats[6] > 0 && stats[13] > 0,
                        "Frontend diagnostic did not exercise required paths");
            passed = true;
        }
        catch (const std::exception &ex)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "%s", ex.what());
        }
    });
    return passed ? 1 : 0;
}

extern "C" TF_HWGS_API int tf_gs_gpu_frontend_probe_v1(const void *inputs, uint32_t count, const void *constants,
                                                       uint32_t batches, const uint32_t *groupEnds, uint32_t groups,
                                                       const void *expected, uint64_t *stats, char *error,
                                                       uint32_t capacity)
{
    return frontendProbe(inputs, count, constants, batches, groupEnds, groups, expected, stats, error, capacity);
}
extern "C" TF_HWGS_API int tf_gs_gpu_dual_merged_probe(const void *inputs, uint32_t count, const void *constants,
                                                       uint32_t batches, const uint32_t *groupEnds, uint32_t groups,
                                                       const void *expected, uint64_t *stats, char *error,
                                                       uint32_t capacity, const void *packed)
{
    if (!packed)
        return 0;
    return frontendProbe(inputs, count, constants, batches, groupEnds, groups, expected, stats, error, capacity,
                         packed);
}
