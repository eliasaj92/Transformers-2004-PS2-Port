// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_gpu_dual_bounds.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class GSDevice12;
struct GSHWDrawConfig;

namespace tf_gpu
{
// Per-field upscaled GS culling in 1/16-pixel units. Offset z is reserved (zero),
// w enables culling; a zero-initialized record leaves it disabled.
struct GrassRasterState
{
    std::array<std::array<int32_t, 4>, 2> scissor{}, offset{};
};
static_assert(sizeof(GrassRasterState) == 64);
struct ReflectionRasterState
{
    std::array<std::array<int32_t, 4>, 4> scissor{}, offset{};
};
static_assert(sizeof(ReflectionRasterState) == 128);
struct ReflectionInput
{
    const void *vertices = nullptr;
    uint32_t count = 0;
    ReflectionRasterState raster;
    // Exactly one representation: expanded diagnostic vertices or the owned
    // compact source packet. The upload copies it before returning.
    const gsnative::DualPacket *source = nullptr;
};
inline constexpr unsigned MaxReflectionComputeGroup = 64;

// GS-thread-only compute/draw component. Destroy Mesh/Batch handles before
// their pool, and the pool before device shutdown or switching APIs.
// Callers still own GS state/texture preparation and VU eligibility checks.
class DualGeometry
{
    struct Page;
    struct Pipeline;
    struct Source;

  public:
    class Mesh
    {
        friend class DualGeometry;
        std::shared_ptr<Source> source;

      public:
        explicit operator bool() const
        {
            return bool(source);
        }
    };
    class Batch
    {
        friend class DualGeometry;
        std::shared_ptr<Page> page;
        uint32_t vertices = 0;
        uint32_t outputOffset = 0;

      public:
        explicit operator bool() const
        {
            return bool(page);
        }
    };
    struct Stats
    {
        uint64_t allocations = 0, reuses = 0, dispatches = 0, draws = 0, bytes = 0, pageProbes = 0;
        uint64_t meshUploads = 0, meshBytes = 0, sourceUploadBytes = 0, constantUploadBytes = 0;
        uint64_t groupedDispatches = 0, groupedMeshes = 0;
        uint64_t grassDispatches = 0, grassBatches = 0;
        uint64_t reflectionDispatches = 0, reflectionVertices = 0, reflectionGroups = 0, reflectionPackets = 0;
    };

    explicit DualGeometry(GSDevice12 &device, uint64_t budget = 128ull * 1024 * 1024,
                          uint64_t meshBudget = 64ull * 1024 * 1024);
    ~DualGeometry();
    DualGeometry(const DualGeometry &) = delete;
    DualGeometry &operator=(const DualGeometry &) = delete;

    // 32-byte source records and 144-byte transform records, as documented by
    // dual_transform.hlsl. One dispatch handles all supplied mesh chunks.
    // Empty means invalid ranges or pool capacity exhausted; no GPU wait or
    // partial dispatch occurs. Qualify finite arithmetic/program identity above.
    Batch prepare(const void *inputs, uint32_t vertices, const void *constants, uint32_t batches);

    // Upload immutable source geometry once to device-local memory. Input chunks
    // must be contiguous, numbered from zero, and contain at most 28 vertices.
    // Handles are explicit: asset identity/invalidation belongs to the caller.
    // The bounded source pool evicts only released, completed meshes. Temporary
    // staging is deferred through the upload fence; there is no forced GPU wait.
    Mesh uploadMesh(const void *inputs, uint32_t vertices, uint32_t batches);
    // Reusing Mesh uploads only transform records, never source vertices.
    // First-source offsets must match the immutable chunk layout exactly.
    Batch prepare(const Mesh &mesh, const void *constants, uint32_t batches);
    // One chunk per resident Mesh, at most eight. Output concatenates meshes
    // in order; only the 144-byte transform records are uploaded. No source
    // copying or GPU readback. Invalid handles/layouts decline before dispatch.
    Batch prepareGroup(const Mesh *meshes, const void *constants, uint32_t count);
    // Grass emitter records: 256 bytes of source constants/scatter, then two
    // initial STQ qwords and two initial RGBA qwords. Each 320-byte record emits
    // 16 contiguous GSVertex records per field. Draw field f of packet p with
    // firstSource=p*16+f*8, sourceCount=8. No geometry readback or forced wait.
    // Field-major output concatenates all packet vertices for each field:
    // firstSource=f*count*8, sourceCount=count*8. The caller must prove that
    // reordering those fields preserves GS rendering and texture dependencies.
    Batch prepareGrass(const void *inputs, uint32_t count, const GrassRasterState *raster = nullptr,
                       bool fieldMajor = false);
    // Qualified reflection records (368 bytes each) emit four interleaved
    // native vertices per source, with no diagnostic raw output or readback.
    // For draw(), each source is two logical pairs: firstSource=first*2,
    // sourceCount=count*2; config indices use 4*vertex+section (0..3).
    // Live ownership and source-derived frontend bounds belong to the caller.
    Batch prepareReflection(const void *inputs, uint32_t count);
    // One 3..22 vertex strip, expanded after GPU GS culling. Each source
    // triangle emits twelve native vertices: (triangle*3+corner)*4+section.
    // For draw(), sourceCount=(count-2)*6. Upscaled, non-AA triangles only.
    Batch prepareReflectionCulled(const void *inputs, uint32_t count, const ReflectionRasterState &raster);
    // Concatenate up to 64 strips in one dispatch. Each threadgroup owns
    // one strip; firstSource advances by (count-2)*6. Draw ordering is unchanged.
    Batch prepareReflectionGroup(const ReflectionInput *inputs, uint32_t count);
    // Whole-mesh metadata from cached source bounds and the two transforms.
    // Chunks in this range must share matrices/bias. Does not map GPU output
    // or transform the mesh's vertices on the CPU.
    bool bounds(const Mesh &mesh, const void *constants, uint32_t batches, uint32_t firstChunk, uint32_t chunks,
                std::array<DualDrawBounds, 2> &result) const;

    // Config indices are local to [firstSource, firstSource + sourceCount).
    // Each source has two interleaved output vertices (indices 2*i + field).
    // Config.nverts must equal sourceCount*2. Retain Batch through its last draw.
    bool draw(const Batch &batch, GSHWDrawConfig &config, uint32_t firstSource, uint32_t sourceCount);
    const Stats &stats() const
    {
        return m_stats;
    }

  private:
    GSDevice12 &m_device;
    std::unique_ptr<Pipeline> m_pipeline;
    std::unique_ptr<Pipeline> m_groupedPipeline;
    std::unique_ptr<Pipeline> m_grassPipeline;
    std::unique_ptr<Pipeline> m_reflectionPipeline;
    std::unique_ptr<Pipeline> m_reflectionCullPipeline;
    std::vector<std::shared_ptr<Page>> m_pages;
    // Small reflection strips share disjoint ranges in one upload/output pair.
    // Retaining the page prevents whole-page reuse until its final slice is
    // issued; Batch handles and the final draw fence protect all earlier slices.
    std::shared_ptr<Page> m_reflectionPage;
    uint32_t m_reflectionUpload = 0, m_reflectionOutput = 0;
    std::size_t m_pageCursor = 0;
    std::vector<std::shared_ptr<Source>> m_sources;
    uint64_t m_budget, m_meshBudget;
    Stats m_stats;
    std::shared_ptr<Page> acquirePage(uint32_t uploadBytes, uint32_t outputBytes);
    Batch dispatch(std::shared_ptr<Page> page, uint64_t sourceAddress, uint32_t vertices, uint32_t constantsOffset,
                   const uint64_t *groupSources = nullptr, uint32_t groups = 0, bool grass = false,
                   bool fieldMajor = false, bool reflection = false, uint32_t reflectionCullGroups = 0,
                   uint32_t outputOffset = 0, uint32_t reflectionSourceVertices = 0);
};
} // namespace tf_gpu
