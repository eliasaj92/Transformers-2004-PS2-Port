// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_grass_geometry.h"
#include <cstddef>

namespace tf_native {
using GrassPacket = std::array<GrassWords, 76>;
struct GrassPacketTemplates {
    std::array<GrassPacket, 2> buffers;
    GrassWords empty;
};

// Read the effective VU input before the manager continuation executes.
// Copies guest templates/preserved lanes; contains no embedded game data.
// This models emitted packets only, not persistent VU registers or memory.
bool prepareGrassPackets(std::span<const uint8_t, 16384> data, uint32_t itop,
                         uint32_t constants, GrassPacketTemplates& output);
void writeGrassPacket(const GrassPacketTemplates& templates, const GrassBatch& batch,
                      std::size_t index, GrassPacket& output);
}
