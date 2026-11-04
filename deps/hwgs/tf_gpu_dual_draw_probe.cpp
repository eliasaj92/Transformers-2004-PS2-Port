// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic rendering of private qualified meshes. Only final images are
// downloaded; the GPU-transformed vertex buffers are never read back.
#include "tf_hwgs.h"
#include "tf_gpu_dual_geometry.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/Common/GSVertex.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace
{
uint32_t word(const void *source)
{
    uint32_t value;
    std::memcpy(&value, source, 4);
    return value;
}
void require(bool condition, const char *text)
{
    if (!condition)
        throw std::runtime_error(text);
}
} // namespace

extern "C" TF_HWGS_API int tf_gs_gpu_dual_draw_probe(const void *inputs, uint32_t count, const void *constants,
                                                     uint32_t batches, const void *expected, uint64_t *stats,
                                                     char *error, uint32_t capacity)
{
    if (!inputs || !constants || !expected || !stats || count < 3 || count > 32768 || !batches || batches > 1024)
        return 0;
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Dual draw probe requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            const auto *transforms = static_cast<const uint8_t *>(constants);
            std::vector<GSVertex> referenceVertices(count * 2);
            std::memcpy(referenceVertices.data(), expected, size_t(count) * 64);
            const auto *reference = reinterpret_cast<const uint8_t *>(referenceVertices.data());
            const unsigned columns = 64, tile = 48, width = columns * tile;
            const unsigned height = ((batches * 4 + columns - 1) / columns) * tile;
            struct Targets
            {
                GSDevice12 *gs;
                GSTexture *gpu = nullptr;
                GSTexture *cpu = nullptr;
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
            require(targets.gpu && targets.cpu, "Cannot allocate dual draw comparison targets");
            const uint32_t uploadBytes = (((count * 32 + 255) & ~255u) + batches * 144 + 65535) & ~65535u;
            const uint32_t outputBytes = (count * 64 + 65535) & ~65535u;
            tf_gpu::DualGeometry pool(*gs, 2ull * (uploadBytes + outputBytes));
            auto first = pool.prepare(inputs, count, constants, batches);
            require(bool(first), "Cannot dispatch first dual geometry batch");
            std::vector<uint8_t> secondInputs(static_cast<const uint8_t *>(inputs),
                                              static_cast<const uint8_t *>(inputs) + count * 32);
            auto secondVertices = referenceVertices;
            auto *secondExpected = reinterpret_cast<uint8_t *>(secondVertices.data());
            for (unsigned i = 0; i < count; ++i)
            {
                for (unsigned lane = 0; lane < 3; ++lane)
                    secondInputs[i * 32 + 28 + lane] ^= 127;
                for (unsigned field = 0; field < 2; ++field)
                    for (unsigned lane = 0; lane < 3; ++lane)
                        secondExpected[(i * 2 + field) * 32 + 8 + lane] ^= 127;
            }
            auto second = pool.prepare(secondInputs.data(), count, constants, batches);
            require(bool(second) && pool.stats().allocations == 2 && pool.stats().reuses == 0,
                    "An outstanding dual batch reused a live buffer");
            require(!pool.prepare(inputs, count, constants, batches) && pool.stats().dispatches == 2,
                    "Geometry pool exceeded its budget or overwrote a retained batch");
            struct Hook
            {
                tf_gpu::DualGeometry &pool;
                GSTexture *target;
                const tf_gpu::DualGeometry::Batch *batch = nullptr;
                unsigned first = 0, count = 0;
                uint64_t replaced = 0, ordinary = 0;
            } hook{pool, targets.gpu};
            struct HookOwner
            {
                GSDevice12 *gs;
                ~HookOwner()
                {
                    gs->SetNativeDrawHandler(nullptr, nullptr);
                }
            } hookOwner{gs};
            gs->SetNativeDrawHandler(&hook, [](void *pointer, GSHWDrawConfig &config) {
                auto &state = *static_cast<Hook *>(pointer);
                if (config.rt != state.target)
                {
                    ++state.ordinary;
                    return false;
                }
                require(state.batch && state.pool.draw(*state.batch, config, state.first, state.count),
                        "Native draw handler rejected qualified GPU geometry");
                ++state.replaced;
                return true;
            });
            const auto draw = [&](const tf_gpu::DualGeometry::Batch &batch, const uint8_t *cpu, unsigned index,
                                  unsigned field, unsigned cell) {
                const unsigned start = word(transforms + index * 144 + 140);
                const unsigned end = index + 1 == batches ? count : word(transforms + (index + 1) * 144 + 140);
                require(start < end && end <= count && end - start <= 28, "Invalid diagnostic mesh range");
                const unsigned n = end - start;
                std::vector<uint16_t> indices;
                for (unsigned i = 2; i < n; ++i)
                {
                    const uint32_t mask = word(transforms + index * 144 + 132 + (i / 16) * 4);
                    if ((mask << (i % 16)) & 32768)
                        continue;
                    indices.push_back(uint16_t(2 * (i - 2) + field));
                    indices.push_back(uint16_t(2 * (i - 1) + field));
                    indices.push_back(uint16_t(2 * i + field));
                }
                if (indices.empty())
                    return;
                unsigned minX = 65535, minY = 65535, maxX = 0, maxY = 0;
                for (unsigned i = 0; i < n; ++i)
                {
                    const uint32_t xy = word(cpu + (start * 2 + i * 2 + field) * 32 + 16);
                    const unsigned x = xy & 65535, y = xy >> 16;
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
                const unsigned tileX = (cell % columns) * tile, tileY = (cell / columns) * tile;
                GSHWDrawConfig config{};
                config.indices = indices.data();
                config.nverts = n * 2;
                config.nindices = static_cast<uint32_t>(indices.size());
                config.indices_per_prim = 3;
                config.topology = GSHWDrawConfig::Topology::Triangle;
                config.scissor = config.drawarea = GSVector4i(tileX, tileY, tileX + tile, tileY + tile);
                config.vs.iip = config.ps.iip = 1;
                config.ps.tfx = 4;
                config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
                config.cb_vs.vertex_scale = GSVector2(2.0f * (tile - 2) / (width * float(maxX - minX + 16)),
                                                      2.0f * (tile - 2) / (height * float(maxY - minY + 16)));
                config.cb_vs.vertex_offset =
                    GSVector2(1.0f - 2.0f * (tileX + 1) / width + minX * config.cb_vs.vertex_scale.x,
                              1.0f - 2.0f * (tileY + 1) / height + minY * config.cb_vs.vertex_scale.y);
                config.cb_vs.max_depth = 0xffffffffu;
                config.rt = targets.gpu;
                config.verts = nullptr;
                hook.batch = &batch;
                hook.first = start;
                hook.count = n;
                gs->RenderHW(config);
                config.rt = targets.cpu;
                config.verts = reinterpret_cast<const GSVertex *>(cpu + start * 64);
                gs->RenderHW(config);
            };
            for (unsigned index = 0; index < batches; ++index)
            {
                for (unsigned field = 0; field < 2; ++field)
                {
                    draw(first, reference, index, field, index * 4 + field);
                    draw(second, secondExpected, index, field, index * 4 + 2 + field);
                }
            }
            // A fence wait exists only in this diagnostic to force the reuse
            // case. The pool adds no forced submission/wait per geometry batch;
            // RenderHW may still roll the renderer's ordinary command lists.
            first = {};
            second = {};
            require(!pool.prepare(inputs, count, constants, batches) && pool.stats().dispatches == 2,
                    "Geometry pool reused a released batch before its final fence completed");
            gs->ExecuteCommandList(true);
            for (unsigned i = 0; i < count; ++i)
            {
                secondInputs[i * 32 + 30] ^= 63;
                for (unsigned field = 0; field < 2; ++field)
                    secondExpected[(i * 2 + field) * 32 + 10] ^= 63;
            }
            auto reused = pool.prepare(secondInputs.data(), count, constants, batches);
            require(bool(reused) && pool.stats().allocations == 2 && pool.stats().reuses == 1,
                    "Completed dual geometry buffers were not reused");
            draw(reused, secondExpected, 0, 0, 0);
            draw(reused, secondExpected, 0, 1, 1);
            std::array<std::vector<uint8_t>, 2> images;
            const GSVector4i area(0, 0, width, height);
            for (unsigned i = 0; i < 2; ++i)
            {
                auto download = gs->CreateDownloadTexture(width, height, GSTexture::Format::Color);
                require(bool(download), "Cannot allocate diagnostic image download");
                download->CopyFromTexture(area, i ? targets.cpu : targets.gpu, area, 0);
                download->Flush();
                images[i].resize(size_t(width) * height * 4);
                require(download->ReadTexels(area, images[i].data(), width * 4), "Cannot read diagnostic image");
            }
            uint64_t nonzero = 0, differing = 0;
            for (size_t pixel = 0; pixel < images[0].size(); pixel += 4)
            {
                nonzero += word(images[0].data() + pixel) != 0;
                differing += std::memcmp(images[0].data() + pixel, images[1].data() + pixel, 4) != 0;
            }
            stats[0] = pool.stats().allocations;
            stats[1] = pool.stats().reuses;
            stats[2] = pool.stats().dispatches;
            stats[3] = pool.stats().draws;
            stats[4] = nonzero;
            stats[5] = differing;
            stats[6] = hook.replaced;
            stats[7] = hook.ordinary;
            require(hook.replaced == pool.stats().draws && hook.ordinary == hook.replaced,
                    "Native draw handler recursion or ordinary submission mismatch");
            require(nonzero > batches * 16ull && !differing,
                    "GPU-resident dual draw image mismatch or empty rendering");
            if (count > 2048)
            {
                tf_gpu::DualGeometry growing(*gs, uint64_t(uploadBytes) + outputBytes);
                auto tiny = growing.prepare(inputs, 3, constants, 1);
                require(bool(tiny), "Cannot allocate small growth-test batch");
                tiny = {};
                gs->ExecuteCommandList(true);
                auto large = growing.prepare(inputs, count, constants, batches);
                require(bool(large) && growing.stats().allocations == 2 &&
                            growing.stats().bytes == uint64_t(uploadBytes) + outputBytes,
                        "Completed small allocations stranded the geometry pool budget");
            }
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
