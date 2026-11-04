// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_geometry.h"
#include "tf_gpu_dual_geometry.h"

namespace tf_gpu
{
struct GrassLiveStats
{
    uint64_t packets = 0, gpuDraws = 0, materializations = 0, declinedFields = 0, boundsDeclined = 0;
    uint64_t outputDeclined = 0;
    uint64_t groupedDispatches = 0, groupedPackets = 0, groupedDraws = 0, groupDeclined = 0;
};
struct GrassPacketView
{
    const gsnative::DualPacket *geometry;
    const uint8_t *packet;
    uint32_t bytes;
};
struct PreparedGrass
{
    const gsnative::DualPacket *geometry = nullptr;
    const uint8_t *packet = nullptr;
    std::array<uint8_t, 320> inputs{};
    GrassRasterState raster{};
    std::array<DualDrawBounds, 2> bounds{};
    DualGeometry::Batch batch;
    uint32_t firstSource = 0;
};
bool grassPacketFraming(const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes);
// Prepare 2..8 consecutive grass packets without consuming GS state. Initial
// color flows from the actual latch, then each preceding packet's retained tail.
// Views and prepared output must remain alive and draws must retain their order.
bool prepareGrassGroup(DualGeometry &pool, const GrassPacketView *views, uint32_t count, PreparedGrass *prepared,
                       GrassLiveStats &stats);
// GS-thread-only. False declines before consuming any packet state. True owns
// both fields, including ordinary fallback and final GS latches. No GPU readback.
bool drawGrassPacket(DualGeometry &pool, const gsnative::DualPacket &geometry, const uint8_t *packet, uint32_t bytes,
                     GrassLiveStats &stats, bool forceFallback = false, const PreparedGrass *prepared = nullptr);
} // namespace tf_gpu
