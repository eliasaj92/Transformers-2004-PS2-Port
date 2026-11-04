// SPDX-License-Identifier: GPL-3.0-or-later
// Private enhanced captures through the real GS texture/depth/blend frontend.
// The comparison uses GIF vertices; native preparation uses only mesh bounds.
#include "tf_hwgs.h"
#include "tf_gpu_dual_geometry.h"
#include "tf_gpu_dual_frontend.h"
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
    std::array<unsigned, 9> sizes{};
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
    const auto push = [&](unsigned serial, uint32_t bytes, unsigned kind) {
        const QueueHeader header{&check, serial, bytes};
        std::memcpy(storage.data(), &header, sizeof(header));
        const uint32_t guard = serial ^ 0xa53f9c17u;
        std::memcpy(storage.data() + bytes - 4, &guard, 4);
        if (!kind)
            return kzgs::QueuePacketCommand(checkQueuedPacket, storage.data(), bytes);
        return kzgs::QueuePacketGroupCommand(kind == 1 ? checkQueuedGroup : checkOtherGroup, storage.data(), bytes);
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
    const bool passed = queued && first && check.valid && check.next == 95;
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
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_frontend_probe_v1(const void *inputs, uint32_t count, const void *constants,
                                                       uint32_t batches, const uint32_t *groupEnds, uint32_t groups,
                                                       const void *expected, uint64_t *stats, char *error,
                                                       uint32_t capacity)
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
            if (grouped)
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
            // Four independent render pairs exercise texture lookup, depth, alpha
            // blending/testing, mipmap selection and an intentional CPU fallback.
            for (unsigned mode = 0; mode < 4; ++mode)
            {
                const uint64_t fallbackBefore = stats[2];
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
                        GIFRegPRIM prim{};
                        prim.PRIM = GS_TRIANGLELIST;
                        prim.IIP = prim.TME = 1;
                        prim.ABE = mode == 1 || mode == 2;
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
                        tex1.MMIN = mode == 2 ? 5 : 1;
                        tex1.MXL = mode == 2 ? 1 : 0;
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
                std::fprintf(stderr,
                             "[gpu:frontend] mode=%u painted=%llu differences=%llu submits=%llu fallback=%llu\n", mode,
                             painted, differences, stats[1], stats[2]);
            }
            device->ExecuteCommandList(true);
            stats[12] = pool.stats().draws;
            stats[14] = pool.stats().groupedDispatches;
            stats[15] = pool.stats().groupedMeshes;
            require(stats[8] == 0 && stats[10] == 0, "Native frontend pixels differ from GIF reference draws");
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
