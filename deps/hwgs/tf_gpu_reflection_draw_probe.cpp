// SPDX-License-Identifier: GPL-3.0-or-later
// Enhanced-renderer diagnostic: original source -> compute -> native draws.
// Only final images are downloaded. Expected vertices belong to the separate
// ordinary draw and diagnostic viewport, never the geometry dispatch.
#include "tf_hwgs.h"
#include "tf_gpu_dual_geometry.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/Common/GSVertex.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
uint32_t word(const void *source)
{
    uint32_t result;
    std::memcpy(&result, source, 4);
    return result;
}
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_reflection_draw_probe(const void *inputs, uint32_t count, const uint32_t *ends,
                                                           uint32_t batches, const void *expected, uint32_t mode,
                                                           uint64_t *stats, char *error, uint32_t capacity)
{
    if (!inputs || !expected || !ends || !stats || !count || count > 65536 || !batches || batches > 2048 || mode > 3)
        return 0;
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12,
                    "Reflection resident draw requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            unsigned previous = 0;
            for (unsigned i = 0; i < batches; ++i)
            {
                require(ends[i] > previous && ends[i] <= count && ends[i] - previous >= 3 && ends[i] - previous <= 28,
                        "Invalid reflection draw source boundaries");
                previous = ends[i];
            }
            require(previous == count, "Reflection draw boundaries do not cover all vertices");
            std::vector<GSVertex> reference(count * 4);
            std::memcpy(reference.data(), expected, size_t(count) * 128);
            const unsigned columns = 64, tile = 48, width = columns * tile;
            const unsigned height = ((batches * 4 + columns - 1) / columns) * tile;
            struct Targets
            {
                GSDevice12 *gs;
                GSTexture *gpu = nullptr, *cpu = nullptr, *texture = nullptr;
                ~Targets()
                {
                    gs->ExecuteCommandList(true);
                    for (auto *texture : {gpu, cpu, texture})
                        if (texture)
                            gs->Recycle(texture);
                }
            } targets{gs};
            targets.gpu = gs->CreateRenderTarget(width, height, GSTexture::Format::Color, true, false);
            targets.cpu = gs->CreateRenderTarget(width, height, GSTexture::Format::Color, true, false);
            targets.texture = gs->CreateTexture(32, 32, 1, GSTexture::Format::Color);
            require(targets.gpu && targets.cpu && targets.texture, "Cannot allocate reflection draw textures");
            std::array<uint32_t, 32 * 32> texels;
            for (unsigned y = 0; y < 32; ++y)
                for (unsigned x = 0; x < 32; ++x)
                    texels[y * 32 + x] = 0x80000000u | (x * 8) | ((y * 8) << 8) | (((x ^ y) * 8) << 16);
            require(targets.texture->Update(GSVector4i(0, 0, 32, 32), texels.data(), 32 * 4),
                    "Cannot upload reflection diagnostic texture");
            const uint64_t pageBytes = uint64_t((count * 368 + 65535) & ~65535u) + ((count * 128 + 65535) & ~65535u);
            tf_gpu::DualGeometry pool(*gs, pageBytes);
            auto batch = pool.prepareReflection(inputs, count);
            require(bool(batch), "Cannot dispatch resident reflection geometry");
            require(!pool.prepareReflection(inputs, count), "Reflection pool overwrote a retained batch");
            uint64_t ordinary = 0;
            const auto draw = [&](unsigned index, const tf_gpu::DualGeometry::Batch &output, const GSVertex *cpu) {
                const unsigned first = index ? ends[index - 1] : 0, n = ends[index] - first;
                for (unsigned section = 0; section < 4; ++section)
                {
                    std::vector<uint16_t> indices;
                    for (unsigned vertex = 2; vertex < n; ++vertex)
                    {
                        const unsigned flags =
                            word(static_cast<const uint8_t *>(inputs) + (first + vertex) * 368 + 356);
                        if (flags & 32768)
                            continue;
                        for (unsigned corner = vertex - 2; corner <= vertex; ++corner)
                            indices.push_back(uint16_t(corner * 4 + section));
                    }
                    if (indices.empty())
                        continue;
                    unsigned minX = 65535, minY = 65535, maxX = 0, maxY = 0;
                    for (unsigned i = 0; i < n; ++i)
                    {
                        const auto &v = cpu[(first + i) * 4 + section];
                        minX = std::min(minX, unsigned(v.XYZ.X));
                        minY = std::min(minY, unsigned(v.XYZ.Y));
                        maxX = std::max(maxX, unsigned(v.XYZ.X));
                        maxY = std::max(maxY, unsigned(v.XYZ.Y));
                    }
                    // Mode 3 overlays the two texture passes in their original
                    // order. Other modes expose each section independently.
                    const unsigned cell = index * 4 + (mode == 3 ? section % 2 : section);
                    const unsigned x = (cell % columns) * tile, y = (cell / columns) * tile;
                    GSHWDrawConfig config{};
                    config.indices = indices.data();
                    config.nverts = n * 4;
                    config.nindices = uint32_t(indices.size());
                    config.indices_per_prim = 3;
                    config.topology = GSHWDrawConfig::Topology::Triangle;
                    config.scissor = config.drawarea = GSVector4i(x, y, x + tile, y + tile);
                    config.vs.iip = config.ps.iip = 1;
                    config.vs.tme = mode != 0;
                    config.ps.tfx = mode ? 0 : 4;
                    config.ps.tcc = 1;
                    config.tex = mode ? targets.texture : nullptr;
                    config.sampler.biln = mode >= 2;
                    config.ps.point_sampler = mode == 1;
                    config.cb_ps.WH = GSVector4(32.0f);
                    config.cb_ps.STScale = GSVector2(1.0f);
                    config.cb_vs.texture_scale = GSVector2(1.0f / (16 * 32));
                    config.colormask.wrgba = 15;
                    if (mode == 3)
                        config.blend = GSHWDrawConfig::BlendState(true, GSDevice::SRC_ALPHA, GSDevice::INV_SRC_ALPHA,
                                                                  GSDevice::OP_ADD, GSDevice::CONST_ONE,
                                                                  GSDevice::CONST_ZERO, false, 0);
                    config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
                    config.cb_vs.vertex_scale = GSVector2(2.0f * (tile - 2) / (width * float(maxX - minX + 16)),
                                                          2.0f * (tile - 2) / (height * float(maxY - minY + 16)));
                    config.cb_vs.vertex_offset =
                        GSVector2(1.0f - 2.0f * (x + 1) / width + minX * config.cb_vs.vertex_scale.x,
                                  1.0f - 2.0f * (y + 1) / height + minY * config.cb_vs.vertex_scale.y);
                    config.cb_vs.max_depth = 0xffffffffu;
                    config.rt = targets.gpu;
                    require(pool.draw(output, config, first * 2, n * 2), "Resident reflection draw declined");
                    config.rt = targets.cpu;
                    config.verts = cpu + first * 4;
                    gs->RenderHW(config);
                    ++ordinary;
                }
            };
            for (unsigned index = 0; index < batches; ++index)
                draw(index, batch, reference.data());
            batch = {};
            require(!pool.prepareReflection(inputs, count),
                    "Reflection pool reused output before its final draw fence");
            gs->ExecuteCommandList(true); // Diagnostic only, to force the reuse case.
            std::vector<uint8_t> swapped(static_cast<const uint8_t *>(inputs),
                                         static_cast<const uint8_t *>(inputs) + count * 368);
            uint64_t changed = 0;
            for (unsigned i = 0; i < count; ++i)
            {
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned column = 0; column < 4; ++column)
                    {
                        auto *p = swapped.data() + i * 368 + 224 + field * 64 + column * 16;
                        std::swap_ranges(p, p + 4, p + 4);
                    }
                for (unsigned section = 0; section < 4; ++section)
                {
                    auto &v = reference[i * 4 + section];
                    changed += i < ends[0] && v.XYZ.X != v.XYZ.Y;
                    std::swap(v.XYZ.X, v.XYZ.Y);
                }
            }
            require(changed > 0, "Reflection transform update did not change the reused draw");
            auto reused = pool.prepareReflection(swapped.data(), count);
            require(bool(reused) && pool.stats().allocations == 1 && pool.stats().reuses == 1,
                    "Completed reflection output page was not reused");
            draw(0, reused, reference.data());
            std::array<std::vector<uint8_t>, 2> images;
            const GSVector4i area(0, 0, width, height);
            for (unsigned i = 0; i < 2; ++i)
            {
                auto download = gs->CreateDownloadTexture(width, height, GSTexture::Format::Color);
                require(bool(download), "Cannot allocate reflection image download");
                download->CopyFromTexture(area, i ? targets.cpu : targets.gpu, area, 0);
                download->Flush();
                images[i].resize(size_t(width) * height * 4);
                require(download->ReadTexels(area, images[i].data(), width * 4),
                        "Cannot read reflection diagnostic image");
            }
            uint64_t painted = 0, differing = 0, colored = 0, imageHash = 14695981039346656037ull;
            for (size_t offset = 0; offset < images[0].size(); offset += 4)
            {
                painted += word(images[0].data() + offset) != 0;
                colored += (word(images[0].data() + offset) & 0xffffffu) != 0;
                differing += std::memcmp(images[0].data() + offset, images[1].data() + offset, 4) != 0;
                for (unsigned channel = 0; channel < 4; ++channel)
                    imageHash = (imageHash ^ images[0][offset + channel]) * 1099511628211ull;
            }
            const auto &s = pool.stats();
            const uint64_t values[] = {s.allocations,
                                       s.reuses,
                                       s.dispatches,
                                       s.draws,
                                       painted,
                                       differing,
                                       uint64_t(width) * height,
                                       s.reflectionDispatches,
                                       s.reflectionVertices,
                                       ordinary,
                                       changed,
                                       colored,
                                       imageHash};
            std::memcpy(stats, values, sizeof(values));
            require(s.reflectionDispatches == 2 && s.reflectionVertices == uint64_t(count) * 2 && ordinary == s.draws &&
                        ordinary > batches * 2ull && colored > batches * 4ull && !differing,
                    "Resident reflection image/count mismatch or empty rendering");
            passed = true;
        }
        catch (const std::exception &exception)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "%s", exception.what());
        }
    });
    return passed ? 1 : 0;
}
