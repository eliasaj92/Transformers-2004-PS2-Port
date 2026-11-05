// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_native_grass_math.h"
#include "runtime/gs/gs_native_reflection_math.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gsnative
{
namespace
{
using Vector = std::array<float, 4>;
float normal(float value)
{
    uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = bits & 0x7f800000u;
    if (!exponent)
        bits &= 0x80000000u;
    else if (exponent == 0x7f800000u)
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    return std::bit_cast<float>(bits);
}
Vector load(const uint8_t *input, unsigned slot)
{
    Vector result;
    std::memcpy(result.data(), input + slot * 16, 16);
    for (auto &lane : result)
        lane = normal(lane);
    return result;
}
Vector add(const Vector &a, const Vector &b)
{
    Vector result;
    for (unsigned lane = 0; lane < 4; ++lane)
        result[lane] = normal(a[lane] + b[lane]);
    return result;
}
Vector scale(const Vector &a, float b)
{
    Vector result;
    for (unsigned lane = 0; lane < 4; ++lane)
        result[lane] = normal(a[lane] * b);
    return result;
}
Vector interpolate(const uint8_t *input, unsigned first, const Vector &weights, bool second)
{
    const Vector edgeA = load(input, first + 1), edgeB = load(input, first + 2);
    auto result = add(load(input, first), scale(edgeA, weights[0]));
    if (second)
        result = add(result, scale(edgeA, weights[2]));
    return add(result, scale(edgeB, weights[second ? 3 : 1]));
}
uint32_t clip(const uint8_t *input, const Vector &position)
{
    auto product = load(input, 0);
    for (unsigned lane = 0; lane < 4; ++lane)
        product[lane] = normal(product[lane] * position[lane]);
    const auto value = add(scale(load(input, 1), position[3]), product);
    const float limit = std::fabs(value[3]);
    uint32_t flags = 0;
    for (unsigned lane = 0; lane < 3; ++lane)
    {
        flags |= uint32_t(value[lane] > limit) << (lane * 2);
        flags |= uint32_t(value[lane] < -limit) << (lane * 2 + 1);
    }
    return flags;
}
uint32_t fixed4(float value)
{
    value *= 16.0f;
    if (value >= 2147483648.0f)
        return 0x7fffffffu;
    if (value <= -2147483648.0f)
        return 0x80000000u;
    return uint32_t(int32_t(value));
}
} // namespace

GrassRawSprite grassRawSprite(const uint8_t *inputs, unsigned sprite, unsigned field)
{
    const bool second = (sprite & 1) != 0;
    const auto weights = load(inputs, 12 + sprite / 2);
    const auto color = interpolate(inputs, 9, weights, second);
    const auto origin = interpolate(inputs, 3 + field * 3, weights, second);
    const auto corner = add(origin, load(inputs, 2));
    uint32_t flags = 0x7fffu + (clip(inputs, origin) & clip(inputs, corner));
    if (flags & 0x8000u)
        flags |= 0xffff0000u;
    const float q = origin[3] == 0.0f
                        ? std::bit_cast<float>((std::bit_cast<uint32_t>(origin[3]) & 0x80000000u) | 0x7f7fffffu)
                        : normal(1.0f / origin[3]);
    const auto a = scale(origin, q), b = scale(corner, q);
    GrassRawSprite result;
    for (unsigned lane = 0; lane < 4; ++lane)
        result.color[lane] = std::bit_cast<uint32_t>(color[lane]);
    for (unsigned lane = 0; lane < 3; ++lane)
    {
        result.xyz[0][lane] = fixed4(second ? a[lane] : b[lane]);
        result.xyz[1][lane] = fixed4(second ? b[lane] : a[lane]);
    }
    result.xyz[0][3] = result.xyz[1][3] = flags;
    return result;
}

void materializeGrassPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes)
{
    if (packet.version != 2)
        return;
    if (!gif || packet.kind != GeometryKind::GrassSprites || packet.vertices != 8 || bytes != 1216)
        throw std::runtime_error("Malformed deferred grass packet");
    DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot restore grass geometry in the required FP mode");
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned sprite = 0; sprite < 6; ++sprite)
        {
            const auto raw = grassRawSprite(packet.inputs.data(), sprite, field);
            auto *output = gif + (6 + field * 38 + sprite * 4) * 16;
            std::memcpy(output, raw.xyz[0].data(), 16);
            std::memcpy(output + 32, raw.color.data(), 16);
            std::memcpy(output + 48, raw.xyz[1].data(), 16);
        }
}

void materializePacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes)
{
    switch (packet.kind)
    {
    case GeometryKind::DualTransform:
        materializeDualPacket(packet, gif, bytes);
        return;
    case GeometryKind::GrassSprites:
        materializeGrassPacket(packet, gif, bytes);
        return;
    case GeometryKind::Reflection:
        materializeReflectionPacket(packet, gif, bytes);
        return;
    }
    throw std::runtime_error("Unknown deferred geometry kind");
}
} // namespace gsnative
