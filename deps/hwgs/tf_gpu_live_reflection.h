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
};
bool reflectionPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// GS thread only. False consumes no state. True owns all four ordered sections,
// fallbacks and final latches. Compute output stays GPU resident.
bool drawReflectionPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet,
                          uint32_t bytes, ReflectionLiveStats &stats, bool forceFallback = false);
} // namespace tf_gpu
