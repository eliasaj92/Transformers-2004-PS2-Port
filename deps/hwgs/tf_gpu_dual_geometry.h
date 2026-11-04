// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

class GSDevice12;
struct GSHWDrawConfig;

namespace tf_gpu
{
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

      public:
        explicit operator bool() const
        {
            return bool(page);
        }
    };
    struct Stats
    {
        uint64_t allocations = 0, reuses = 0, dispatches = 0, draws = 0, bytes = 0;
        uint64_t meshUploads = 0, meshBytes = 0, sourceUploadBytes = 0, constantUploadBytes = 0;
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
    std::vector<std::shared_ptr<Page>> m_pages;
    std::vector<std::shared_ptr<Source>> m_sources;
    uint64_t m_budget, m_meshBudget;
    Stats m_stats;
    std::shared_ptr<Page> acquirePage(uint32_t uploadBytes, uint32_t outputBytes);
    Batch dispatch(std::shared_ptr<Page> page, uint64_t sourceAddress, uint32_t vertices, uint32_t constantsOffset);
};
} // namespace tf_gpu
