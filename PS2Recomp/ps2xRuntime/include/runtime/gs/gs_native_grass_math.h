// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gs_native_dual_math.h"

namespace gsnative
{
struct GrassRawSprite
{
    std::array<std::array<uint32_t, 4>, 2> xyz{};
    std::array<uint32_t, 4> color{};
};
// Requires DualFloatScope, a qualified 256-byte emitter record, sprite < 8 and
// field < 2. These are raw VU words, before GS packing or sprite culling.
GrassRawSprite grassRawSprite(const uint8_t *inputs, unsigned sprite, unsigned field);
// Resolves the first three pairs, including the prologue's delayed stores.
// The final CPU pair, framing, texture words and padding stay intact. Never
// reads the GPU.
void materializeGrassPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes);
// Ordinary renderer paths must dispatch by kind before reading deferred GIF.
void materializePacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes);
} // namespace gsnative
