// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_geometry.h"

namespace tf_gpu
{
class DualGeometry;
struct GrassLiveStats
{
    uint64_t packets = 0, gpuDraws = 0, materializations = 0, declinedFields = 0, boundsDeclined = 0;
    uint64_t outputDeclined = 0;
};
bool grassPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// GS-thread-only. False declines before consuming any packet state. True owns
// both fields, including ordinary fallback and final GS latches. No GPU readback.
bool drawGrassPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes,
                     GrassLiveStats &stats, bool forceFallback = false);
} // namespace tf_gpu
