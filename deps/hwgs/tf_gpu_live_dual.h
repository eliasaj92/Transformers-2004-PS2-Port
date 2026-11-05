// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_geometry.h"
#include "tf_gpu_dual_geometry.h"

namespace tf_gpu
{
struct DualPacketView
{
    const gsnative::DualPacket *geometry;
    const uint8_t *packet;
    uint32_t bytes;
};
struct PreparedDual
{
    DualGeometry::Mesh mesh;
    DualGeometry::Batch batch;
    std::array<DualDrawBounds, 2> bounds;
    uint32_t first = 0;
};
struct DualMergeStats
{
    uint64_t groups = 0, packets = 0, gpuDraws = 0, materializations = 0, ordinaryFields = 0;
    uint64_t stateDeclined = 0, feedbackDeclined = 0, boundsDeclined = 0, outputDeclined = 0;
    uint64_t nativeBatches = 0, nativePackets = 0, cacheHits = 0, cacheUploads = 0, largestBatch = 0;
};
inline constexpr unsigned MaxDualMeshPackets = 64;
// A resident mesh owns immutable source vertices and strip topology. Dynamic
// matrices are supplied per submission; no guest pointer survives the call.
// Destroy the cache before its pool, on the GS thread.
class DualMeshCache
{
  public:
    struct Prepared
    {
        DualGeometry::Mesh mesh;
        std::array<std::vector<uint16_t>, 2> indices;
        std::vector<uint8_t> constants;
        std::vector<bool> triangleChunks;
        std::array<DualDrawBounds, 2> bounds;
        uint32_t vertices = 0;
    };
    explicit DualMeshCache(DualGeometry &pool);
    ~DualMeshCache();
    Prepared *prepare(const DualPacketView *views, uint32_t count, DualMergeStats &stats);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Transport grouping is not proof of engine asset identity. Bounds are shared
// only across adjacent chunks with both transforms and texture bias equal.
bool dualMeshTransformsMatch(const DualPacketView &a, const DualPacketView &b);
bool dualMeshStateMatches(const DualPacketView &a, const DualPacketView &b);
bool dualPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// GS-thread-only. Two to eight adjacent packets, one independently resident
// source per packet. False consumes no GS state. True owns both fields and all
// fallbacks. Reordering requires disjoint scissors and no target feedback.
bool drawDualGroup(DualGeometry &pool, const DualPacketView *views, const PreparedDual *prepared, uint32_t count,
                   DualMergeStats &stats, bool forceFallback = false, DualMeshCache *cache = nullptr);
} // namespace tf_gpu
