// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_reflection_math.h"
#include "tf_gpu_dual_geometry.h"

namespace tf_gpu
{
struct ReflectionLiveStats
{
    uint64_t packets = 0, gpuDraws = 0, materializations = 0, declinedSections = 0;
    uint64_t boundsDeclined = 0, outputDeclined = 0, boundsNodes = 0, boundsLeaves = 0;
    uint64_t preparedGroups = 0, preparedPackets = 0, groupDeclined = 0;
};
struct ReflectionPacketView
{
    const gsnative::DualPacket *geometry;
    const uint8_t *packet;
    uint32_t bytes;
};
struct PreparedReflection
{
    std::array<DualDrawBounds, 4> bounds;
    ReflectionRasterState raster;
    DualGeometry::Batch batch;
    uint32_t firstSource = 0;
};
// Source-only preparation; consumes no GS state. On success, use each prepared
// entry in the original packet order. No textures, registers or draws reorder.
bool prepareReflectionGroup(DualGeometry &pool, const ReflectionPacketView *packets, uint32_t count,
                            PreparedReflection *prepared, ReflectionLiveStats &stats);
bool reflectionPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// GS thread only. False consumes no state. True owns all four ordered sections,
// fallbacks and final latches. Compute output stays GPU resident.
bool drawReflectionPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet,
                          uint32_t bytes, ReflectionLiveStats &stats, bool forceFallback = false,
                          const PreparedReflection *prepared = nullptr);
} // namespace tf_gpu
