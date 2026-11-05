// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gs_native_dual_math.h"

namespace gsnative
{
constexpr unsigned ReflectionMaxVertices = 22;
constexpr unsigned ReflectionLightingOffset = 720;
constexpr unsigned ReflectionInputStride = 368;
static_assert(ReflectionMaxVertices * 32 <= ReflectionLightingOffset && ReflectionLightingOffset + 176 == 28 * 32);
// Compact source record: position.xyz, original S, normal.xyz, original T.
// constants: two projection matrices, draw opacity, two restart masks, zero.
struct ReflectionLitVertex
{
    std::array<uint32_t, 4> stq{}, color{}, position{};
};
struct ReflectionRawVertex
{
    std::array<uint32_t, 4> stq{}, color{}, xyz{};
};
// Arithmetic requires DualFloatScope. These are source-only fallback helpers;
// ordinary native rendering gets the actual geometry from the GPU kernel.
ReflectionLitVertex reflectionLitVertex(const DualPacket &packet, unsigned vertex);
ReflectionRawVertex reflectionProjectVertex(const DualPacket &packet, const ReflectionLitVertex &lit, unsigned vertex,
                                            unsigned section);
bool reflectionSourceValid(const DualPacket &packet);
bool expandReflectionInputs(const DualPacket &packet, uint8_t *output, unsigned bytes);
void materializeReflectionPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes);
} // namespace gsnative
