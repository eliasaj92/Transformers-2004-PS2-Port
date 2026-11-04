// SPDX-License-Identifier: GPL-3.0-or-later
// Private enhanced captures through the real GS texture/depth/blend frontend.
// Bounds use only source constants. Captured output supplies the comparison,
// fallback, and a diagnostic depth witness; live ownership remains separate.
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
    const tf_gpu::GrassRasterState *raster = nullptr;
    unsigned field = 0;
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
    // The native API expects GS primitive culling to have been applied before
    // submission. Match the GPU's zero-area records for a CPU fallback.
    const auto &offset = s.raster->offset[s.field];
    const GSVector4i scissor = GSVector4i::load<false>(s.raster->scissor[s.field].data());
    for (unsigned i = 0; i < count; i += 2)
    {
        const auto &a = vertices[i].XYZ;
        const auto &b = vertices[i + 1].XYZ;
        GSVector4i box(std::min(a.X, b.X) - offset[0], std::min(a.Y, b.Y) - offset[1], std::max(a.X, b.X) - offset[0],
                       std::max(a.Y, b.Y) - offset[1]);
        box -= ((box & GSVector4i(15)) == GSVector4i(0)) & GSVector4i(0, 0, 1, 1);
        if (box.rempty() || !(box + GSVector4i(0, 0, 1, 1)).rintersects(scissor))
            vertices[i + 1].XYZ = vertices[i].XYZ;
    }
}
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_grass_frontend_probe(const void *inputs, uint32_t count, const void *expected,
                                                          uint64_t *stats, char *error, uint32_t capacity)
{
    if (!inputs || !expected || !stats || !count || count > 1024)
        return 0;
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Frontend probe requires D3D12");
            auto *renderer = GSRendererHW::GetInstance();
            auto *device = GSDevice12::GetInstance();
            std::vector<GSVertex> reference(count * 32);
            std::memcpy(reference.data(), expected, size_t(count) * 1024);
            tf_gpu::DualGeometry pool(*device);
            std::vector<tf_gpu::GrassRasterState> raster(count);
            for (auto &state : raster)
                for (unsigned field = 0; field < 2; ++field)
                {
                    state.scissor[field] = {-8, (field ? 224 : 0) * 16 - 8, 511 * 16 + 8, (field ? 447 : 223) * 16 + 8};
                    state.offset[field] = {28671, field ? 25591 : 29183, 0, 1};
                }
            auto output = pool.prepareGrass(inputs, count, raster.data());
            require(bool(output), "Cannot dispatch frontend grass geometry");
            std::vector<std::array<tf_gpu::DualDrawBounds, 2>> bounds(count);
            std::vector<bool> bounded(count);
            for (unsigned packet = 0; packet < count; ++packet)
            {
                bounded[packet] =
                    tf_gpu::projectGrassBounds(static_cast<const uint8_t *>(inputs) + packet * 320, bounds[packet]);
                ++stats[bounded[packet] ? 16 : 17];
            }
            const std::vector<uint16_t> indices{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
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
                        prim.PRIM = GS_SPRITE;
                        prim.TME = 1;
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
                        tex1.LCM = mode == 2;
                        tex1.K = mode == 2 ? 16 : 0;
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
                        for (unsigned packet = 0; packet < count; ++packet)
                        {
                            const GSVertex *vertices = reference.data() + packet * 32 + field * 16;
                            bool varyingDepth = false;
                            GSVertexTrace::Vertex minimum{}, maximum{};
                            // The renderer bounds never inspect transformed vertices.
                            // This diagnostic independently witnesses nonconstant depth.
                            for (unsigned i = 3; i < 16; i += 2)
                                varyingDepth |= float(vertices[i].XYZ.Z) != float(vertices[1].XYZ.Z);
                            Submission submission{&pool, &output, vertices,  packet * 16 + field * 8,
                                                  8,     stats,   mode == 3, &raster[packet],
                                                  field};
                            if (!side && varyingDepth && bounded[packet] &&
                                tf_gpu::prepareGrassTrace(bounds[packet][field], *renderer->m_context, *renderer->PRIM,
                                                          minimum, maximum))
                            {
                                if (!mode && !field && !packet)
                                {
                                    auto invalid = minimum;
                                    invalid.t.z = 2.0f;
                                    require(!renderer->DrawNativeSprites(invalid, maximum, indices.data(), 16, true,
                                                                         &submission, submit, materialize),
                                            "Non-unit sprite Q was accepted");
                                    require(!renderer->DrawNativeSprites(minimum, maximum, indices.data(), 16, false,
                                                                         &submission, submit, materialize),
                                            "Missing sprite depth witness was accepted");
                                    auto reversed = indices;
                                    std::swap(reversed[0], reversed[1]);
                                    require(!renderer->DrawNativeSprites(minimum, maximum, reversed.data(), 16, true,
                                                                         &submission, submit, materialize),
                                            "Nonconsecutive sprite pairs were accepted");
                                    require(stats[1] == 0 && stats[2] == 0,
                                            "Declined sprites invoked native callbacks");
                                }
                                require(renderer->DrawNativeSprites(minimum, maximum, indices.data(), 16, true,
                                                                    &submission, submit, materialize),
                                        "Renderer declined qualified diagnostic sprites");
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
                {
                    const bool different = std::memcmp(images[0].data() + pixel, images[1].data() + pixel, 4) != 0;
                    if (different && fullDifferences < 4)
                        std::fprintf(stderr, "[gpu:grass-frontend] full pixel=%zu native=%08x gif=%08x\n", pixel / 4,
                                     word(images[0].data() + pixel), word(images[1].data() + pixel));
                    fullDifferences += different;
                }
                stats[10] += fullDifferences;
                stats[11] += uint64_t(imageWidth) * imageHeight;
                std::fprintf(stderr, "[gpu:grass-frontend] mode=%u image=%ux%u full_differences=%llu\n", mode,
                             imageWidth, imageHeight, fullDifferences);
                renderer->ReadbackTextureCache();
                uint64_t painted = 0, differences = 0;
                for (unsigned y = 0; y < 448; ++y)
                    for (unsigned x = 0; x < 512; ++x)
                    {
                        const auto actual = renderer->m_mem.ReadPixel32(x, y, 0, 8);
                        const auto expectedPixel = renderer->m_mem.ReadPixel32(x, y, 112 * 32, 8);
                        painted += actual != 0x20102010;
                        if (actual != expectedPixel && differences < 8)
                            std::fprintf(stderr, "[gpu:grass-frontend] mode=%u pixel=%u,%u native=%08x gif=%08x\n",
                                         mode, x, y, actual, expectedPixel);
                        differences += actual != expectedPixel;
                    }
                stats[7] += painted;
                stats[8] += differences;
                ++stats[9];
                require(painted > 1000, "Frontend comparison did not paint enough pixels");
                require(mode == 3 || stats[2] == fallbackBefore,
                        "A GPU sprite mode unexpectedly materialized CPU vertices");
                std::fprintf(stderr,
                             "[gpu:grass-frontend] mode=%u painted=%llu differences=%llu submits=%llu fallback=%llu\n",
                             mode, painted, differences, stats[1], stats[2]);
            }
            device->ExecuteCommandList(true);
            stats[12] = pool.stats().draws;
            stats[14] = pool.stats().grassDispatches;
            stats[15] = pool.stats().grassBatches;
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
