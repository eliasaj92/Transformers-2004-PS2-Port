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
#include <bit>
#include <cfenv>
#include <cmath>
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

extern "C" TF_HWGS_API int tf_gs_gpu_dual_draw_probe_v3(const void *inputs, uint32_t count, const void *constants,
                                                        uint32_t batches, const uint32_t *groupEnds, uint32_t groups,
                                                        const void *expected, uint64_t *stats, char *error,
                                                        uint32_t capacity)
{
    if (!inputs || !constants || !expected || !stats || !groupEnds || !groups || groups > batches || count < 3 ||
        count > 32768 || !batches || batches > 1024)
        return 0;
    bool passed = false;
    kzgs::RunOnGSThread([&] {
        try
        {
            require(g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12, "Dual draw probe requires D3D12");
            auto *gs = GSDevice12::GetInstance();
            const auto *transforms = static_cast<const uint8_t *>(constants);
            unsigned previous = 0;
            for (unsigned group = 0; group < groups; ++group)
            {
                require(groupEnds[group] > previous && groupEnds[group] <= batches, "Invalid diagnostic mesh groups");
                previous = groupEnds[group];
            }
            require(previous == batches, "Diagnostic groups do not cover every chunk");
            std::vector<GSVertex> referenceVertices(count * 2);
            std::memcpy(referenceVertices.data(), expected, size_t(count) * 64);
            const auto *reference = reinterpret_cast<const uint8_t *>(referenceVertices.data());
            const unsigned columns = 64, tile = 48, width = columns * tile;
            const unsigned height = ((groups * 4 + columns - 1) / columns) * tile;
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
            const uint32_t uploadBytes = (batches * 144 + 65535) & ~65535u;
            const uint32_t outputBytes = (count * 64 + 65535) & ~65535u;
            const uint32_t sourceBytes = (count * 32 + 65535) & ~65535u;
            tf_gpu::DualGeometry pool(*gs, 2ull * (uploadBytes + outputBytes), 2ull * sourceBytes);
            auto firstMesh = pool.uploadMesh(inputs, count, batches);
            require(bool(firstMesh), "Cannot upload first resident mesh");
            uint64_t boundedGroups = 0, declinedBounds = 0, boundedVertices = 0, wrappedBounds = 0;
            double maxAreaRatio = 0;
            std::vector<std::array<tf_gpu::DualDrawBounds, 2>> groupBounds(groups);
            for (unsigned group = 0; group < groups; ++group)
            {
                const unsigned firstChunk = group ? groupEnds[group - 1] : 0, lastChunk = groupEnds[group];
                const unsigned start = word(transforms + firstChunk * 144 + 140);
                const unsigned end = lastChunk == batches ? count : word(transforms + lastChunk * 144 + 140);
                auto &bounds = groupBounds[group];
                const auto csr = _mm_getcsr();
                const bool accepted =
                    pool.bounds(firstMesh, constants, batches, firstChunk, lastChunk - firstChunk, bounds);
                require(csr == _mm_getcsr(), "Bounds preparation changed the caller's floating-point state");
                if (!accepted)
                {
                    ++declinedBounds;
                    continue;
                }
                ++boundedGroups;
                for (unsigned field = 0; field < 2; ++field)
                {
                    const auto &b = bounds[field];
                    wrappedBounds += b.wrappedAxes != 0;
                    unsigned minXY[2] = {65535, 65535}, maxXY[2] = {};
                    for (unsigned i = start; i < end; ++i)
                    {
                        const auto *v = reference + (i * 2 + field) * 32;
                        const uint32_t xy = word(v + 16), xyz[] = {xy & 65535, xy >> 16, word(v + 20)};
                        const double stq[] = {std::bit_cast<float>(word(v)), std::bit_cast<float>(word(v + 4)),
                                              std::bit_cast<float>(word(v + 12))};
                        for (unsigned axis = 0; axis < 3; ++axis)
                        {
                            require(xyz[axis] >= b.xyzMin[axis] && xyz[axis] <= b.xyzMax[axis],
                                    "Captured GS coordinate escaped conservative mesh bounds");
                            require(stq[axis] >= b.stq[axis].low && stq[axis] <= b.stq[axis].high,
                                    "Captured STQ escaped conservative mesh bounds");
                        }
                        for (unsigned axis = 0; axis < 2; ++axis)
                        {
                            const double texture = stq[axis] / stq[2];
                            require(texture >= b.texture[axis].low && texture <= b.texture[axis].high,
                                    "Captured texture coordinate escaped conservative mesh bounds");
                            minXY[axis] = std::min(minXY[axis], xyz[axis]);
                            maxXY[axis] = std::max(maxXY[axis], xyz[axis]);
                        }
                        for (unsigned channel = 0; channel < 4; ++channel)
                        {
                            const unsigned color = (word(v + 8) >> (channel * 8)) & 255;
                            require(color >= b.colorMin[channel] && color <= b.colorMax[channel],
                                    "Captured color escaped cached mesh bounds");
                        }
                        ++boundedVertices;
                    }
                    const double area = double(b.xyzMax[0] - b.xyzMin[0] + 1) * (b.xyzMax[1] - b.xyzMin[1] + 1);
                    const double actual = double(maxXY[0] - minXY[0] + 1) * (maxXY[1] - minXY[1] + 1);
                    maxAreaRatio = std::max(maxAreaRatio, area / actual);
                }
            }
            require(boundedGroups == groups, "Diagnostic meshes require unambiguous conservative bounds");
            {
                struct EnvironmentRestore
                {
                    fenv_t environment;
                    uint32_t csr = _mm_getcsr();
                    EnvironmentRestore()
                    {
                        require(std::fegetenv(&environment) == 0, "Cannot save FP environment");
                    }
                    ~EnvironmentRestore()
                    {
                        std::fesetenv(&environment);
                        _mm_setcsr(csr);
                    }
                } restore;
                require(std::fesetround(FE_TOWARDZERO) == 0, "Cannot set VU-style rounding for metadata check");
                _mm_setcsr(_mm_getcsr() | 0x8040u);
                const uint32_t expectedCsr = _mm_getcsr();
                std::array<tf_gpu::DualDrawBounds, 2> checked;
                const bool accepted = pool.bounds(firstMesh, constants, batches, 0, groupEnds[0], checked);
                require(_mm_getcsr() == expectedCsr && std::fegetround() == FE_TOWARDZERO,
                        "Metadata preparation did not preserve VU-style floating-point controls");
                require(accepted && checked[0].xyzMin == groupBounds[0][0].xyzMin &&
                            checked[0].xyzMax == groupBounds[0][0].xyzMax,
                        "Metadata depends on the caller's rounding controls");
            }
            {
                std::vector<uint8_t> crossingW(transforms, transforms + batches * 144);
                for (unsigned chunk = 0; chunk < groupEnds[0]; ++chunk)
                    for (unsigned field = 0; field < 2; ++field)
                        for (unsigned column = 0; column < 4; ++column)
                            std::memset(crossingW.data() + chunk * 144 + field * 64 + column * 16 + 12, 0, 4);
                std::array<tf_gpu::DualDrawBounds, 2> rejected;
                require(!pool.bounds(firstMesh, crossingW.data(), batches, 0, groupEnds[0], rejected),
                        "Mesh metadata accepted a zero perspective denominator");
                std::vector<uint8_t> malformed(static_cast<const uint8_t *>(inputs),
                                               static_cast<const uint8_t *>(inputs) + 3 * 32);
                const uint32_t nan = 0x7fc00000;
                std::memcpy(malformed.data(), &nan, 4);
                tf_gpu::DualAttributes attributes;
                require(!tf_gpu::summarizeDualInputs(malformed.data(), 3, attributes) && !attributes.valid,
                        "Nonfinite source geometry produced usable metadata");
            }
            std::vector<uint8_t> badConstants(transforms, transforms + batches * 144);
            badConstants[140] ^= 1;
            require(!pool.prepare(firstMesh, badConstants.data(), batches) && !pool.stats().dispatches,
                    "Resident mesh accepted changed chunk boundaries");
            auto first = pool.prepare(firstMesh, constants, batches);
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
            auto secondMesh = pool.uploadMesh(secondInputs.data(), count, batches);
            require(bool(secondMesh) && pool.stats().meshUploads == 2, "Cannot upload second resident mesh");
            require(!pool.uploadMesh(inputs, count, batches) && pool.stats().meshUploads == 2,
                    "Resident source pool exceeded its budget or evicted a retained mesh");
            auto second = pool.prepare(secondMesh, constants, batches);
            require(bool(second) && pool.stats().allocations == 2 && pool.stats().reuses == 0,
                    "An outstanding dual batch reused a live buffer");
            require(!pool.prepare(firstMesh, constants, batches) && pool.stats().dispatches == 2,
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
            uint64_t expectedOrdinary = 0;
            const auto draw = [&](const tf_gpu::DualGeometry::Batch &batch, const uint8_t *cpu, unsigned index,
                                  unsigned field, unsigned cell) {
                const unsigned firstChunk = index ? groupEnds[index - 1] : 0;
                const unsigned lastChunk = groupEnds[index];
                const auto chunkStart = [&](unsigned chunk) {
                    return chunk == batches ? count : word(transforms + chunk * 144 + 140);
                };
                const unsigned start = chunkStart(firstChunk), end = chunkStart(lastChunk);
                require(start < end && end <= count, "Invalid diagnostic mesh range");
                const unsigned n = end - start;
                std::vector<uint16_t> indices;
                const auto appendChunk = [&](unsigned chunk, unsigned origin, std::vector<uint16_t> &output) {
                    const unsigned first = chunkStart(chunk), last = chunkStart(chunk + 1);
                    require(first >= start && first < last && last <= end && last - first <= 28,
                            "Invalid diagnostic chunk range");
                    for (unsigned i = 2; i < last - first; ++i)
                    {
                        const uint32_t mask = word(transforms + chunk * 144 + 132 + (i / 16) * 4);
                        if ((mask << (i % 16)) & 32768)
                            continue;
                        for (unsigned corner = i - 2; corner <= i; ++corner)
                            output.push_back(uint16_t(2 * (first - origin + corner) + field));
                    }
                };
                for (unsigned chunk = firstChunk; chunk < lastChunk; ++chunk)
                    appendChunk(chunk, start, indices);
                if (indices.empty())
                    return;
                // Prepare the diagnostic viewport from cached object metadata,
                // independently of transformed CPU-reference vertices.
                const auto &bounds = groupBounds[index][field];
                const unsigned minX = bounds.xyzMin[0], minY = bounds.xyzMin[1];
                const unsigned maxX = bounds.xyzMax[0], maxY = bounds.xyzMax[1];
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
                // Preserve the individual chunk submissions on the comparison
                // side. Matching grouped-vs-grouped draws would not check the
                // aggregation's strip boundaries or primitive order.
                for (unsigned chunk = firstChunk; chunk < lastChunk; ++chunk)
                {
                    const unsigned first = chunkStart(chunk), last = chunkStart(chunk + 1);
                    std::vector<uint16_t> localIndices;
                    appendChunk(chunk, first, localIndices);
                    if (localIndices.empty())
                        continue;
                    config.indices = localIndices.data();
                    config.nindices = static_cast<uint32_t>(localIndices.size());
                    config.nverts = (last - first) * 2;
                    config.verts = reinterpret_cast<const GSVertex *>(cpu + first * 64);
                    gs->RenderHW(config);
                    ++expectedOrdinary;
                }
            };
            for (unsigned index = 0; index < groups; ++index)
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
            require(!pool.prepare(firstMesh, constants, batches) && pool.stats().dispatches == 2,
                    "Geometry pool reused a released batch before its final fence completed");
            gs->ExecuteCommandList(true);
            // Swap projected X/Y only in the second field. This exercises both
            // equal-field reuse and different-field computation in the shader.
            std::vector<uint8_t> swappedTransforms(transforms, transforms + batches * 144);
            for (unsigned chunk = 0; chunk < batches; ++chunk)
                for (unsigned column = 0; column < 4; ++column)
                {
                    auto begin = swappedTransforms.begin() + chunk * 144 + 64 + column * 16;
                    std::swap_ranges(begin, begin + 4, begin + 4);
                }
            auto reusedVertices = secondVertices;
            uint64_t changedCoordinates = 0;
            const unsigned changedRange =
                2 * (groupEnds[0] == batches ? count : word(transforms + groupEnds[0] * 144 + 140));
            for (unsigned i = 1; i < reusedVertices.size(); i += 2)
            {
                auto &vertex = reusedVertices[i];
                changedCoordinates += i < changedRange && vertex.XYZ.X != vertex.XYZ.Y;
                std::swap(vertex.XYZ.X, vertex.XYZ.Y);
            }
            require(changedCoordinates > 0, "Transform update diagnostic did not change geometry");
            const auto *reusedExpected = reinterpret_cast<const uint8_t *>(reusedVertices.data());
            auto reused = pool.prepare(secondMesh, swappedTransforms.data(), batches);
            require(bool(reused) && pool.stats().allocations == 2 && pool.stats().reuses == 1,
                    "Completed dual geometry buffers were not reused");
            require(pool.stats().meshUploads == 2 && pool.stats().sourceUploadBytes == uint64_t(count) * 64 &&
                        pool.stats().constantUploadBytes == uint64_t(batches) * 144 * 3,
                    "Resident mesh preparation uploaded source vertices again");
            require(!pool.uploadMesh(inputs, count, batches), "Completed retained mesh was evicted");
            require(pool.bounds(secondMesh, swappedTransforms.data(), batches, 0, groupEnds[0], groupBounds[0]),
                    "Changed transform did not produce conservative draw metadata");
            draw(reused, reusedExpected, 0, 0, 0);
            draw(reused, reusedExpected, 0, 1, 1);
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
            stats[8] = pool.stats().meshUploads;
            stats[9] = pool.stats().meshBytes;
            stats[10] = pool.stats().sourceUploadBytes;
            stats[11] = pool.stats().constantUploadBytes;
            stats[12] = boundedGroups;
            stats[13] = declinedBounds;
            stats[14] = boundedVertices;
            stats[15] = wrappedBounds;
            stats[16] = static_cast<uint64_t>(std::ceil(maxAreaRatio * 1000000));
            require(hook.replaced == pool.stats().draws && hook.ordinary == expectedOrdinary,
                    "Native draw handler recursion or ordinary submission mismatch");
            require(nonzero > groups * 16ull && !differing, "GPU-resident dual draw image mismatch or empty rendering");
            if (count > 2048)
            {
                const uint32_t combinedUploadBytes = (((count * 32 + 255) & ~255u) + batches * 144 + 65535) & ~65535u;
                tf_gpu::DualGeometry growing(*gs, uint64_t(combinedUploadBytes) + outputBytes, sourceBytes);
                require(!growing.prepare(firstMesh, constants, batches), "Foreign pool's mesh was accepted");
                auto tiny = growing.prepare(inputs, 3, constants, 1);
                require(bool(tiny), "Cannot allocate small growth-test batch");
                tiny = {};
                gs->ExecuteCommandList(true);
                auto large = growing.prepare(inputs, count, constants, batches);
                require(bool(large) && growing.stats().allocations == 2 &&
                            growing.stats().bytes == uint64_t(combinedUploadBytes) + outputBytes,
                        "Completed small allocations stranded the geometry pool budget");
                auto smallMesh = growing.uploadMesh(inputs, 3, 1);
                require(bool(smallMesh), "Cannot upload small source growth-test mesh");
                smallMesh = {};
                require(!growing.uploadMesh(inputs, count, batches), "Source pool evicted an in-flight mesh");
                gs->ExecuteCommandList(true);
                auto largeMesh = growing.uploadMesh(inputs, count, batches);
                require(bool(largeMesh) && growing.stats().meshUploads == 2 && growing.stats().meshBytes == sourceBytes,
                        "Completed small source allocation stranded the source pool budget");
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
