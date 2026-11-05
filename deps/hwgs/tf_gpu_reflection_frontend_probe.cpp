// SPDX-License-Identifier: GPL-3.0-or-later
// Source-only native preparation versus captured accelerated GIF, through the
// actual GS texture/depth/blend frontend. Only final images are downloaded.
#include "tf_hwgs.h"
#include "tf_gpu_live_reflection.h"
#include "kzgs_internal.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
constexpr unsigned MaxBytes = 4 * (4 + 3 * gsnative::ReflectionMaxVertices) * 16;
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
uint64_t wide(const void *p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
struct alignas(16) Qword
{
    uint64_t low, high;
};
void transfer(std::vector<Qword> &p)
{
    p[0] = {uint64_t(p.size() - 1) | (1ull << 15) | (1ull << 60), GIF_REG_A_D};
    kzgs::ExecuteGIFOnGSThread(2, reinterpret_cast<const uint8_t *>(p.data()), uint32_t(p.size() * 16));
}
template <typename T> void reg(std::vector<Qword> &p, unsigned address, const T &value)
{
    p.push_back({wide(&value), address});
}
void clearTarget(unsigned side)
{
    GIFRegPRIM prim{};
    prim.PRIM = GS_SPRITE;
    GIFRegFRAME frame{};
    frame.FBP = side * 112;
    frame.FBW = 8;
    GIFRegZBUF z{};
    z.ZBP = 224 + side * 112;
    z.PSM = PSMZ32;
    GIFRegTEST test{};
    test.ZTE = 1;
    test.ZTST = ZTST_ALWAYS;
    GIFRegSCISSOR scissor{};
    scissor.SCAX1 = 511;
    scissor.SCAY1 = 447;
    std::vector<Qword> p(1);
    p.push_back({1, 0x1a});
    reg(p, 0x00, prim);
    reg(p, 0x4c, frame);
    reg(p, 0x4e, z);
    reg(p, 0x47, test);
    p.push_back({0, 0x18});
    reg(p, 0x40, scissor);
    p.push_back({0x20102010, 0x01});
    p.push_back({0, 0x05});
    p.push_back({uint64_t(512 * 16) | (uint64_t(448 * 16) << 16), 0x05});
    transfer(p);
    g_gs_renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
}
void state(unsigned side, unsigned mode)
{
    std::vector<Qword> p(1);
    p.push_back({1, 0x1a});
    p.push_back({1, 0x46});
    p.push_back({0, 0x22});
    for (unsigned context = 0; context < 2; ++context)
    {
        GIFRegFRAME frame{};
        frame.FBP = side * 112;
        frame.FBW = 8;
        GIFRegZBUF z{};
        z.ZBP = 224 + side * 112;
        z.PSM = PSMZ32;
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
        tex.TBP0 = 15360 + context * 128;
        tex.TBW = 1;
        tex.TW = tex.TH = 6;
        tex.TCC = 1;
        GIFRegTEX1 filter{};
        filter.MMAG = mode != 0;
        filter.MMIN = mode == 2 ? 5 : mode != 0;
        filter.MXL = mode == 2;
        GIFRegMIPTBP1 mip{};
        mip.TBP1 = tex.TBP0 + 64;
        mip.TBW1 = 1;
        reg(p, 0x4c + context, frame);
        reg(p, 0x4e + context, z);
        reg(p, 0x47 + context, test);
        reg(p, 0x42 + context, alpha);
        reg(p, 0x06 + context, tex);
        reg(p, 0x14 + context, filter);
        reg(p, 0x34 + context, mip);
        p.push_back({0, 0x08 + context});
    }
    transfer(p);
}
std::vector<uint8_t> image(GSDevice12 *device, unsigned side, uint32_t &width, uint32_t &height)
{
    const unsigned base = side * 112 * 32;
    auto *target = g_texture_cache->GetExactTarget(base, 8, GSTextureCache::RenderTarget, base + 1);
    require(target && target->m_texture && target->m_TEX0.TBP0 == base, "Missing reflection frontend target");
    auto *texture = target->m_texture;
    width = texture->GetWidth();
    height = texture->GetHeight();
    require(width >= 2048 && height >= 1792, "Reflection target is below required internal resolution");
    const GSVector4i area(0, 0, width, height);
    auto download = device->CreateDownloadTexture(width, height, GSTexture::Format::Color);
    require(bool(download), "Cannot download reflection frontend image");
    download->CopyFromTexture(area, texture, area, 0);
    download->Flush();
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    require(download->ReadTexels(area, pixels.data(), width * 4), "Cannot read reflection frontend image");
    return pixels;
}
} // namespace

extern "C" TF_HWGS_API int tf_gs_reflection_frontend_probe(const gsnative::DualPacket *sources, const uint8_t *captured,
                                                           uint32_t count, uint32_t mode, uint64_t *stats, char *error,
                                                           uint32_t capacity)
{
    if (!sources || !captured || !stats || !count || count > 1024 || mode > 3)
        return 0;
    std::fill_n(stats, 12, 0);
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Reflection frontend needs D3D12");
            auto *renderer = GSRendererHW::GetInstance();
            auto *device = GSDevice12::GetInstance();
            tf_gpu::DualGeometry pool(*device);
            tf_gpu::ReflectionLiveStats counters;
            std::vector<GSVertex> latches(count);
            // Several independent render pairs limit occlusion between captured batches.
            for (unsigned begin = 0; begin < count; begin += 192)
            {
                renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
                renderer->ReadbackTextureCache();
                GIFRegBITBLTBUF blit{};
                blit.DBW = 8;
                blit.DPSM = PSMCT32;
                std::memset(renderer->m_mem.m_vm8, 0, 4 * 1024 * 1024);
                renderer->InvalidateVideoMem(blit, GSVector4i(0, 0, 512, 2048));
                for (unsigned context = 0; context < 2; ++context)
                    for (unsigned lod = 0; lod < 2; ++lod)
                    {
                        const unsigned size = 64 >> lod, base = 15360 + context * 128 + lod * 64;
                        for (unsigned y = 0; y < size; ++y)
                            for (unsigned x = 0; x < size; ++x)
                            {
                                const uint32_t color = ((x / 4) ^ (y / 4)) & 1 ? 0x807040e0 : 0x4040e070;
                                renderer->m_mem.WritePixel32(x, y, color ^ (context * 0x202000) ^ (lod * 0x2020), base,
                                                             1);
                            }
                    }
                for (unsigned side = 0; side < 2; ++side)
                {
                    clearTarget(side);
                    state(side, mode);
                    for (unsigned i = begin; i < std::min(begin + 192, count); ++i)
                    {
                        const auto &source = sources[i];
                        const unsigned n = source.vertices, bytes = 4 * (4 + 3 * n) * 16;
                        require(bytes <= MaxBytes && source.version == 2, "Invalid reflection source packet");
                        alignas(16) std::array<uint8_t, MaxBytes> packet;
                        std::memcpy(packet.data(), captured + i * MaxBytes, bytes);
                        for (unsigned section = 0; section < 4; ++section)
                        {
                            auto *f = packet.data() + section * (4 + n * 3) * 16;
                            uint64_t tag = wide(f + 48);
                            GIFRegPRIM prim{};
                            prim.U64 = tag >> 47 & 0x7ff;
                            prim.IIP = prim.TME = 1;
                            prim.FST = prim.AA1 = 0;
                            prim.ABE = mode == 1 || mode == 2;
                            tag = (tag & ~(0x7ffull << 47)) | (prim.U64 << 47);
                            std::memcpy(f + 48, &tag, 8);
                            if (!side)
                                for (unsigned v = 0; v + 3 < n; ++v)
                                    std::memset(f + 64 + v * 48, 0xcd, 48);
                        }
                        require(tf_gpu::reflectionPacketFraming(source, packet.data(), bytes),
                                "Unexpected reflection framing");
                        if (!side)
                        {
                            if (!tf_gpu::drawReflectionPacket(pool, source, packet.data(), bytes, counters, mode == 3))
                            {
                                gsnative::materializeReflectionPacket(source, packet.data(), bytes);
                                kzgs::ExecuteGIFOnGSThread(1, packet.data(), bytes);
                                ++stats[9];
                            }
                            latches[i] = renderer->GetNativeVertexLatch();
                        }
                        else
                        {
                            kzgs::ExecuteGIFOnGSThread(1, packet.data(), bytes);
                            require(!std::memcmp(&latches[i], &renderer->GetNativeVertexLatch(), sizeof(GSVertex)),
                                    "Reflection native path changed final GS latch");
                            ++stats[6];
                        }
                        require(!renderer->m_path[0].nloop, "Reflection left incomplete PATH1 packet");
                    }
                    renderer->Flush(GSState::GSFlushReason::CONTEXTCHANGE);
                }
                uint32_t w0, h0, w1, h1;
                const auto a = image(device, 0, w0, h0), b = image(device, 1, w1, h1);
                require(w0 == w1 && h0 == h1, "Reflection target dimensions differ");
                for (size_t p = 0; p < a.size(); p += 4)
                    if (std::memcmp(a.data() + p, b.data() + p, 4))
                    {
                        if (stats[4] < 8)
                        {
                            uint32_t actual, expected;
                            std::memcpy(&actual, a.data() + p, 4);
                            std::memcpy(&expected, b.data() + p, 4);
                            std::fprintf(stderr,
                                         "[reflection:frontend] mode=%u group=%u pixel=%zu,%zu native=%08x gif=%08x\n",
                                         mode, begin, p / 4 % w0, p / 4 / w0, actual, expected);
                        }
                        ++stats[4];
                    }
                stats[5] += uint64_t(w0) * h0;
                renderer->ReadbackTextureCache();
                for (unsigned y = 0; y < 448; ++y)
                    for (unsigned x = 0; x < 512; ++x)
                        stats[7] += renderer->m_mem.ReadPixel32(x, y, 0, 8) != 0x20102010;
                ++stats[8];
            }
            device->ExecuteCommandList(true);
            stats[0] = counters.packets;
            stats[1] = counters.gpuDraws;
            stats[2] = counters.materializations;
            stats[3] = counters.declinedSections;
            stats[10] = pool.stats().reflectionDispatches;
            stats[11] = pool.stats().reflectionVertices;
            require(stats[4] == 0, "Reflection frontend pixels differ from accelerated GIF");
            require(stats[7] > 1000 && stats[6] == count,
                    "Reflection frontend did not exercise visible batches/latches");
            require(mode == 3 ? stats[1] == 0 && stats[2] > 0 : stats[1] > 100,
                    "Reflection frontend did not exercise requested GPU/fallback path");
            passed = true;
        }
        catch (const std::exception &e)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "%s", e.what());
        }
    });
    return passed ? 1 : 0;
}
