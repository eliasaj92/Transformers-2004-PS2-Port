// SPDX-License-Identifier: GPL-3.0-or-later
// GPU-resident sprite input, compared with decoded enhanced capture vertices.
// Only final diagnostic images are downloaded; geometry remains on the GPU.
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
uint32_t word(const void *data)
{
    uint32_t value;
    std::memcpy(&value, data, 4);
    return value;
}
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_grass_draw_probe(const void *inputs, uint32_t count, const void *expected,
                                                      uint64_t *stats, char *error, uint32_t capacity)
{
    if (!inputs || !expected || !stats || !count || count > 1024)
        return 0;
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Grass draw requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            const unsigned columns = 64, tile = 32, width = columns * tile;
            const unsigned height = ((count * 4 + columns - 1) / columns) * tile;
            struct Targets
            {
                GSDevice12 *gs;
                GSTexture *gpu = nullptr, *cpu = nullptr;
                ~Targets()
                {
                    gs->ExecuteCommandList(true);
                    if (gpu)
                        gs->Recycle(gpu);
                    if (cpu)
                        gs->Recycle(cpu);
                }
            } targets{gs};
            targets.gpu = gs->CreateRenderTarget(width, height, GSTexture::Format::Color, true, false);
            targets.cpu = gs->CreateRenderTarget(width, height, GSTexture::Format::Color, true, false);
            require(targets.gpu && targets.cpu, "Cannot allocate grass comparison targets");
            std::vector<GSVertex> reference(count * 32);
            std::memcpy(reference.data(), expected, size_t(count) * 1024);
            const uint32_t uploadBytes = (count * 384 + 65535) & ~65535u;
            const uint32_t outputBytes = (count * 1024 + 65535) & ~65535u;
            tf_gpu::DualGeometry pool(*gs, 2ull * (uploadBytes + outputBytes));
            auto first = pool.prepareGrass(inputs, count);
            require(bool(first), "Cannot prepare resident grass geometry");
            auto changed = reference;
            std::vector<uint8_t> secondInputs(static_cast<const uint8_t *>(inputs),
                                              static_cast<const uint8_t *>(inputs) + size_t(count) * 320);
            for (unsigned i = 0; i < count; ++i)
            {
                auto *bias = secondInputs.data() + i * 320 + 9 * 16;
                uint32_t value = word(bias);
                require((value & 0xff800000u) == 0x4b000000u && value < 0x4b7fff00u,
                        "Grass color is outside the diagnostic translation range");
                ++value;
                std::memcpy(bias, &value, 4);
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned vertex = 1; vertex < 16; ++vertex)
                        ++changed[i * 32 + field * 16 + vertex].RGBAQ.R;
            }
            auto second = pool.prepareGrass(secondInputs.data(), count);
            require(bool(second) && pool.stats().allocations == 2, "Retained grass output was overwritten");
            require(!pool.prepareGrass(inputs, count) && pool.stats().dispatches == 2,
                    "Grass pool exceeded its budget while both outputs are retained");

            struct Hook
            {
                tf_gpu::DualGeometry &pool;
                GSTexture *target;
                const tf_gpu::DualGeometry::Batch *batch = nullptr;
                unsigned first = 0;
                uint64_t replaced = 0, ordinary = 0;
            } hook{pool, targets.gpu};
            struct HookOwner
            {
                GSDevice12 *gs;
                ~HookOwner()
                {
                    gs->SetNativeDrawHandler(nullptr, nullptr);
                }
            } owner{gs};
            gs->SetNativeDrawHandler(&hook, [](void *pointer, GSHWDrawConfig &config) {
                auto &h = *static_cast<Hook *>(pointer);
                if (config.rt != h.target)
                {
                    ++h.ordinary;
                    return false;
                }
                require(h.batch && h.pool.draw(*h.batch, config, h.first, 8), "GPU sprite input declined");
                ++h.replaced;
                return true;
            });
            std::array<uint16_t, 16> indices;
            for (unsigned i = 0; i < indices.size(); ++i)
                indices[i] = uint16_t(i);
            const auto draw = [&](const tf_gpu::DualGeometry::Batch &batch, const GSVertex *cpu, unsigned index,
                                  unsigned field, unsigned cell) {
                const auto *vertices = cpu + index * 32 + field * 16;
                unsigned minX = 65535, minY = 65535, maxX = 0, maxY = 0;
                for (unsigned i = 0; i < 16; ++i)
                {
                    minX = std::min(minX, unsigned(vertices[i].XYZ.X));
                    minY = std::min(minY, unsigned(vertices[i].XYZ.Y));
                    maxX = std::max(maxX, unsigned(vertices[i].XYZ.X));
                    maxY = std::max(maxY, unsigned(vertices[i].XYZ.Y));
                }
                const unsigned tileX = (cell % columns) * tile, tileY = (cell / columns) * tile;
                GSHWDrawConfig config{};
                config.indices = indices.data();
                config.nverts = 16;
                config.nindices = 48;
                config.indices_per_prim = 6;
                config.topology = GSHWDrawConfig::Topology::Triangle;
                config.vs.expand = GSHWDrawConfig::VSExpand::Sprite;
                config.scissor = config.drawarea = GSVector4i(tileX, tileY, tileX + tile, tileY + tile);
                config.ps.tfx = 4;
                config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
                config.cb_vs.vertex_scale = GSVector2(2.0f * (tile - 2) / (width * float(maxX - minX + 16)),
                                                      2.0f * (tile - 2) / (height * float(maxY - minY + 16)));
                config.cb_vs.vertex_offset =
                    GSVector2(1.0f - 2.0f * (tileX + 1) / width + minX * config.cb_vs.vertex_scale.x,
                              1.0f - 2.0f * (tileY + 1) / height + minY * config.cb_vs.vertex_scale.y);
                config.cb_vs.max_depth = 0xffffffffu;
                if (cell == 0)
                {
                    require(gs->CanRenderHWFromGPU(config), "Qualified sprite layout declined");
                    auto invalid = config;
                    invalid.nindices -= 3;
                    require(!gs->CanRenderHWFromGPU(invalid), "Incomplete expanded sprite count accepted");
                    invalid = config;
                    invalid.vs.expand = GSHWDrawConfig::VSExpand::Line;
                    require(!gs->CanRenderHWFromGPU(invalid), "Unqualified line expansion accepted");
                    std::swap(indices[0], indices[1]);
                    require(!gs->CanRenderHWFromGPU(config), "Nonsequential fixed-expansion indices accepted");
                    std::swap(indices[0], indices[1]);
                }
                config.rt = targets.gpu;
                config.verts = nullptr;
                hook.batch = &batch;
                hook.first = index * 16 + field * 8;
                gs->RenderHW(config);
                // Alternating ordinary sprites verify that the root SRV no
                // longer points at GPU output after the native scope returns.
                config.rt = targets.cpu;
                config.verts = vertices;
                gs->RenderHW(config);
                if ((cell & 127u) == 127u)
                    gs->ExecuteCommandList(false);
            };
            for (unsigned i = 0; i < count; ++i)
                for (unsigned field = 0; field < 2; ++field)
                {
                    draw(first, reference.data(), i, field, i * 4 + field);
                    draw(second, changed.data(), i, field, i * 4 + field + 2);
                }
            first = {};
            second = {};
            gs->ExecuteCommandList(true);
            auto reused = pool.prepareGrass(secondInputs.data(), count);
            require(bool(reused) && pool.stats().allocations == 2 && pool.stats().reuses == 1,
                    "Completed grass page was not reused");
            draw(reused, changed.data(), 0, 0, 0);
            draw(reused, changed.data(), 0, 1, 1);
            std::array<std::vector<uint8_t>, 2> images;
            const GSVector4i area(0, 0, width, height);
            for (unsigned i = 0; i < 2; ++i)
            {
                auto download = gs->CreateDownloadTexture(width, height, GSTexture::Format::Color);
                require(bool(download), "Cannot allocate grass image download");
                download->CopyFromTexture(area, i ? targets.cpu : targets.gpu, area, 0);
                download->Flush();
                images[i].resize(size_t(width) * height * 4);
                require(download->ReadTexels(area, images[i].data(), width * 4), "Cannot read grass image");
            }
            uint64_t painted = 0, differing = 0;
            for (size_t offset = 0; offset < images[0].size(); offset += 4)
            {
                painted += word(images[0].data() + offset) != 0;
                differing += std::memcmp(images[0].data() + offset, images[1].data() + offset, 4) != 0;
            }
            stats[0] = pool.stats().allocations;
            stats[1] = pool.stats().reuses;
            stats[2] = pool.stats().dispatches;
            stats[3] = pool.stats().draws;
            stats[4] = painted;
            stats[5] = differing;
            stats[6] = hook.replaced;
            stats[7] = hook.ordinary;
            stats[8] = pool.stats().grassDispatches;
            stats[9] = pool.stats().grassBatches;
            stats[10] = uint64_t(width) * height;
            require(hook.replaced == count * 4 + 2 && hook.ordinary == hook.replaced &&
                        hook.replaced == pool.stats().draws,
                    "Sprite submission count or scope mismatch");
            require(painted > count * 4ull && !differing, "GPU sprite image mismatch or empty rendering");
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
