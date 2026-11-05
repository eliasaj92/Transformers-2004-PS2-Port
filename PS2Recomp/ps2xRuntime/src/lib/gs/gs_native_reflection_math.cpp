// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_native_reflection_math.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gsnative
{
namespace
{
uint32_t word(const void *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}
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
float load(const void *p)
{
    return normal(std::bit_cast<float>(word(p)));
}
float add(float a, float b)
{
    return normal(normal(a) + normal(b));
}
float mul(float a, float b)
{
    return normal(normal(a) * normal(b));
}
float reciprocal(float value)
{
    value = normal(value);
    return value == 0 ? std::bit_cast<float>((std::bit_cast<uint32_t>(value) & 0x80000000u) | 0x7f7fffffu)
                      : normal(1.0f / value);
}
uint32_t integer(float value)
{
    if (value >= 2147483648.0f)
        return 0x7fffffffu;
    if (value <= -2147483648.0f)
        return 0x80000000u;
    return uint32_t(int32_t(value));
}
using Vector = std::array<float, 3>;
Vector vector(const uint8_t *p)
{
    return {load(p), load(p + 4), load(p + 8)};
}
Vector plus(Vector a, Vector b)
{
    return {add(a[0], b[0]), add(a[1], b[1]), add(a[2], b[2])};
}
Vector times(Vector a, Vector b)
{
    return {mul(a[0], b[0]), mul(a[1], b[1]), mul(a[2], b[2])};
}
Vector scale(Vector a, float b)
{
    return times(a, {b, b, b});
}
float dot(Vector a, Vector b)
{
    const auto product = times(a, b);
    return add(add(product[0], product[1]), product[2]);
}
} // namespace

ReflectionLitVertex reflectionLitVertex(const DualPacket &packet, unsigned vertex)
{
    const auto *source = packet.inputs.data() + vertex * 32;
    const auto *constants = packet.inputs.data() + ReflectionLightingOffset;
    const auto p = vector(source), n = vector(source + 16);
    const auto c0 = vector(constants + 6 * 16), c1 = vector(constants + 10 * 16);
    const float a = std::max(dot(n, vector(constants + 5 * 16)), 0.0f);
    const float b = std::max(dot(n, vector(constants + 9 * 16)), 0.0f);
    const auto diffuse = plus(scale(c1, b), scale(c0, a));
    const float ambient = load(constants + 9 * 16 + 12);
    const auto light = plus(scale(diffuse, load(constants + 5 * 16 + 12)), {ambient, ambient, ambient});
    const auto material = times(vector(constants + 7 * 16), c0);
    const auto rgb = scale(times(material, light), 128.0f);
    const auto eye = plus(vector(constants + 4 * 16), {-p[0], -p[1], -p[2]});
    const auto view = scale(eye, reciprocal(normal(std::sqrt(std::fabs(dot(eye, eye))))));
    const float facing = dot(n, view);
    const auto reflected = plus(scale(plus(n, n), facing), {-view[0], -view[1], -view[2]});
    const auto transformed =
        plus(plus(scale(vector(constants), reflected[0]), scale(vector(constants + 16), reflected[1])),
             scale(vector(constants + 32), reflected[2]));
    const float length = normal(std::sqrt(dot(transformed, transformed)));
    const auto direction = scale(transformed, length == 0 ? length : reciprocal(length));
    const float denominator = add(direction[2], 1.0f);
    const float q = mul(0.5f, reciprocal(denominator));
    const float opacity =
        mul(mul(255.0f, load(constants + 8 * 16)), mul(add(1.0f, -facing), load(constants + 8 * 16 + 4)));
    ReflectionLitVertex result;
    result.stq = {std::bit_cast<uint32_t>(add(mul(direction[0], q), 0.5f)),
                  std::bit_cast<uint32_t>(add(mul(direction[1], q), 0.5f)), std::bit_cast<uint32_t>(denominator),
                  word(source + 28)};
    for (unsigned i = 0; i < 3; ++i)
    {
        result.color[i] = integer(std::clamp(rgb[i], 0.0f, 255.0f));
        result.position[i] = word(source + i * 4);
    }
    result.color[3] = integer(opacity);
    result.position[3] = word(source + 12);
    return result;
}

ReflectionRawVertex reflectionProjectVertex(const DualPacket &packet, const ReflectionLitVertex &lit, unsigned vertex,
                                            unsigned section)
{
    const auto *matrix = packet.constants.data() + (section % 2) * 64;
    float projected[4];
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        float value = load(matrix + 48 + lane * 4);
        for (int column = 2; column >= 0; --column)
            value = add(value, mul(load(matrix + column * 16 + lane * 4), std::bit_cast<float>(lit.position[column])));
        projected[lane] = value;
    }
    const float q = reciprocal(projected[3]);
    ReflectionRawVertex result;
    const unsigned flags = (word(packet.constants.data() + 132 + (vertex / 16) * 4) << (vertex % 16)) & 65535u;
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        const float tex = section < 2 ? std::bit_cast<float>(lit.stq[axis])
                          : axis == 0 ? std::bit_cast<float>(lit.position[3])
                          : axis == 1 ? std::bit_cast<float>(lit.stq[3])
                                      : 1.0f;
        result.stq[axis] = std::bit_cast<uint32_t>(mul(tex, q));
        result.color[axis] = section < 2 ? (int32_t(lit.color[3]) < 0 ? 0 : lit.color[3]) : lit.color[axis];
        result.xyz[axis] = integer(mul(projected[axis], q) * 16.0f);
    }
    result.stq[3] = lit.stq[3];
    result.color[3] = integer(mul(load(packet.constants.data() + 128), 128.0f));
    result.xyz[3] = (flags & 32768) ? flags | 0xffff0000u : flags;
    return result;
}

bool reflectionSourceValid(const DualPacket &packet)
{
    if (packet.kind != GeometryKind::Reflection || (packet.version != 1 && packet.version != 2) ||
        packet.vertices < 3 || packet.vertices > ReflectionMaxVertices || word(packet.constants.data() + 140))
        return false;
    const auto finite = [](const uint8_t *p, unsigned bytes) {
        for (unsigned i = 0; i < bytes; i += 4)
            if ((word(p + i) & 0x7f800000u) == 0x7f800000u)
                return false;
        return true;
    };
    return finite(packet.inputs.data(), packet.vertices * 32) &&
           finite(packet.inputs.data() + ReflectionLightingOffset, 176) && finite(packet.constants.data(), 132);
}

bool expandReflectionInputs(const DualPacket &packet, uint8_t *output, unsigned bytes)
{
    if (!output || !reflectionSourceValid(packet) || bytes < packet.vertices * ReflectionInputStride)
        return false;
    for (unsigned vertex = 0; vertex < packet.vertices; ++vertex)
    {
        auto *p = output + vertex * ReflectionInputStride;
        const auto *source = packet.inputs.data() + vertex * 32;
        std::memset(p, 0, ReflectionInputStride);
        std::memcpy(p, packet.inputs.data() + ReflectionLightingOffset, 176);
        std::memcpy(p + 176, source + 12, 4);
        std::memcpy(p + 180, source + 28, 4);
        std::memcpy(p + 192, source + 16, 12);
        std::memcpy(p + 208, source, 12);
        std::memcpy(p + 224, packet.constants.data(), 132);
        const unsigned flags = (word(packet.constants.data() + 132 + (vertex / 16) * 4) << (vertex % 16)) & 65535u;
        std::memcpy(p + 356, &flags, 4);
    }
    return true;
}

void materializeReflectionPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes)
{
    if (packet.version != 2)
        return;
    if (!gif || !reflectionSourceValid(packet) || bytes != 4 * (4 + packet.vertices * 3) * 16)
        throw std::runtime_error("Malformed deferred reflection packet");
    DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot restore reflection geometry in the required FP mode");
    for (unsigned vertex = 0; vertex < packet.vertices; ++vertex)
    {
        const auto lit = reflectionLitVertex(packet, vertex);
        for (unsigned section = 0; section < 4; ++section)
        {
            const auto raw = reflectionProjectVertex(packet, lit, vertex, section);
            auto *destination = gif + (section * (4 + packet.vertices * 3) + 4 + vertex * 3) * 16;
            std::memcpy(destination, raw.stq.data(), section == 0 ? 16 : 12);
            std::memcpy(destination + 16, raw.color.data(), 16);
            std::memcpy(destination + 32, raw.xyz.data(), 16);
        }
    }
}
} // namespace gsnative
