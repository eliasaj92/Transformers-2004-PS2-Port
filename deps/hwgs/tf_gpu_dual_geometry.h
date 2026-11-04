// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

class GSDevice12;
struct GSHWDrawConfig;

namespace tf_gpu
{
// GS-thread-only compute/draw component. The device must outlive this object
// and every Batch. Destroy them before renderer shutdown or switching APIs.
// Callers still own GS state/texture preparation and VU eligibility checks.
class DualGeometry
{
    struct Page;
    struct Pipeline;

  public:
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
    };

    explicit DualGeometry(GSDevice12 &device, uint64_t budget = 128ull * 1024 * 1024);
    ~DualGeometry();
    DualGeometry(const DualGeometry &) = delete;
    DualGeometry &operator=(const DualGeometry &) = delete;

    // 32-byte source records and 144-byte transform records, as documented by
    // dual_transform.hlsl. One dispatch handles all supplied mesh chunks.
    // Empty means invalid ranges or pool capacity exhausted; no GPU wait or
    // partial dispatch occurs. Qualify finite arithmetic/program identity above.
    Batch prepare(const void *inputs, uint32_t vertices, const void *constants, uint32_t batches);

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
    uint64_t m_budget;
    Stats m_stats;
};
} // namespace tf_gpu
