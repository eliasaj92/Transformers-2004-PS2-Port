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
};
bool dualPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// GS-thread-only. Two to eight adjacent packets, one independently resident
// source per packet. False consumes no GS state. True owns both fields and all
// fallbacks. Reordering requires disjoint scissors and no target feedback.
bool drawDualGroup(DualGeometry &pool, const DualPacketView *views, const PreparedDual *prepared, uint32_t count,
                   DualMergeStats &stats, bool forceFallback = false);
} // namespace tf_gpu
