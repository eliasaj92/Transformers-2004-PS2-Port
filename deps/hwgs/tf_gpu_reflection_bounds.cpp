// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_bounds.h"
#include "runtime/gs/gs_native_reflection_math.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>

namespace tf_gpu
{
namespace
{
using Range = DualRange;
using Vector = std::array<Range, 3>;
float load(const uint8_t *p)
{
    uint32_t bits;
    std::memcpy(&bits, p, 4);
    if (!(bits & 0x7f800000u))
        bits &= 0x80000000u;
    return std::bit_cast<float>(bits);
}
Range scalar(float value)
{
    return {value, value};
}
Range unite(Range a, Range b)
{
    return {std::min(a.low, b.low), std::max(a.high, b.high)};
}
Range negative(Range a)
{
    return {-a.high, -a.low};
}
bool finite(Range a)
{
    return std::isfinite(a.low) && std::isfinite(a.high) && a.low <= a.high;
}
// Endpoints are binary32 values. Each monotone operation uses the VU's
// RTZ/FTZ environment; /fp:strict prevents contraction and constant folding
// across the independently rounded multiply/add/sqrt/reciprocal operations.
Range plus(Range a, Range b)
{
    volatile float low = float(a.low) + float(b.low), high = float(a.high) + float(b.high);
    return {low, high};
}
Range times(Range a, Range b)
{
    volatile float aa = float(a.low) * float(b.low), ab = float(a.low) * float(b.high);
    volatile float ba = float(a.high) * float(b.low), bb = float(a.high) * float(b.high);
    const double v[] = {aa, ab, ba, bb};
    return {*std::min_element(std::begin(v), std::end(v)), *std::max_element(std::begin(v), std::end(v))};
}
Range squared(Range a)
{
    const float low = a.low <= 0 && a.high >= 0 ? 0.0f : float(std::min(std::abs(a.low), std::abs(a.high)));
    const float high = float(std::max(std::abs(a.low), std::abs(a.high)));
    volatile float a2 = low * low, b2 = high * high;
    return {a2, b2};
}
Range dot(const Vector &a, const Vector &b)
{
    return plus(plus(times(a[0], b[0]), times(a[1], b[1])), times(a[2], b[2]));
}
bool normalize(const Vector &input, Vector &output)
{
    const auto length = plus(plus(squared(input[0]), squared(input[1])), squared(input[2]));
    if (!finite(length) || length.low <= 0)
        return false;
    volatile float low = std::sqrt(float(length.low)), high = std::sqrt(float(length.high));
    volatile float inverseLow = 1.0f / high, inverseHigh = 1.0f / low;
    const Range inverse{inverseLow, inverseHigh};
    if (!finite(inverse))
        return false;
    for (unsigned axis = 0; axis < 3; ++axis)
        output[axis] = times(input[axis], inverse);
    return true;
}

struct SourceBounds
{
    const gsnative::DualPacket &packet;
    std::array<unsigned, gsnative::ReflectionMaxVertices> indices{};
    uint32_t nodes = 0, leaves = 0;
    bool lighting(unsigned first, unsigned count, Vector &stq)
    {
        ++nodes;
        Vector position{}, normal{};
        const auto *constants = packet.inputs.data() + gsnative::ReflectionLightingOffset;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto *p = packet.inputs.data() + indices[first + i] * 32;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                const auto a = scalar(load(p + axis * 4)), b = scalar(load(p + 16 + axis * 4));
                position[axis] = i ? unite(position[axis], a) : a;
                normal[axis] = i ? unite(normal[axis], b) : b;
            }
        }
        const auto evaluate = [&] {
            Vector eye, view, reflected, transformed, direction;
            for (unsigned axis = 0; axis < 3; ++axis)
                eye[axis] = plus(scalar(load(constants + 64 + axis * 4)), negative(position[axis]));
            if (!normalize(eye, view))
                return false;
            const auto facing = dot(normal, view);
            for (unsigned axis = 0; axis < 3; ++axis)
                reflected[axis] = plus(times(plus(normal[axis], normal[axis]), facing), negative(view[axis]));
            for (unsigned axis = 0; axis < 3; ++axis)
                transformed[axis] = plus(plus(times(scalar(load(constants + axis * 4)), reflected[0]),
                                              times(scalar(load(constants + 16 + axis * 4)), reflected[1])),
                                         times(scalar(load(constants + 32 + axis * 4)), reflected[2]));
            if (!normalize(transformed, direction))
                return false;
            stq[2] = plus(direction[2], scalar(1.0f));
            if (!finite(stq[2]) || stq[2].low <= 0)
                return false;
            volatile float low = 1.0f / float(stq[2].high), high = 1.0f / float(stq[2].low);
            const auto q = times(scalar(0.5f), {low, high});
            for (unsigned axis = 0; axis < 2; ++axis)
                stq[axis] = plus(times(direction[axis], q), scalar(0.5f));
            return finite(stq[0]) && finite(stq[1]);
        };
        if (evaluate())
        {
            ++leaves;
            return true;
        }
        if (count == 1)
            return false;
        // Tighten only an inconclusive source box. The split uses object-space
        // normals/positions, never projected vertices or GPU output.
        unsigned lane = 4;
        double spread = -1;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (normal[axis].high - normal[axis].low > spread)
            {
                spread = normal[axis].high - normal[axis].low;
                lane = 4 + axis;
            }
        if (spread == 0)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (position[axis].high - position[axis].low > spread)
                {
                    spread = position[axis].high - position[axis].low;
                    lane = axis;
                }
        const unsigned half = count / 2;
        std::nth_element(indices.begin() + first, indices.begin() + first + half, indices.begin() + first + count,
                         [&](unsigned a, unsigned b) {
                             return load(packet.inputs.data() + a * 32 + lane * 4) <
                                    load(packet.inputs.data() + b * 32 + lane * 4);
                         });
        Vector a, b;
        if (!lighting(first, half, a) || !lighting(first + half, count - half, b))
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            stq[axis] = unite(a[axis], b[axis]);
        return true;
    }
};
} // namespace

bool projectReflectionBounds(const gsnative::DualPacket &packet, std::array<DualDrawBounds, 4> &result,
                             uint32_t *visitedNodes, uint32_t *acceptedLeaves)
{
    result = {};
    if (visitedNodes)
        *visitedNodes = 0;
    if (acceptedLeaves)
        *acceptedLeaves = 0;
    if (!gsnative::reflectionSourceValid(packet))
        return false;
    gsnative::DualFloatScope floating;
    if (!floating.ready)
        return false;
    SourceBounds source{packet};
    std::iota(source.indices.begin(), source.indices.end(), 0);
    Vector reflectionStq;
    const bool accepted = source.lighting(0, packet.vertices, reflectionStq);
    if (visitedNodes)
        *visitedNodes = source.nodes;
    if (acceptedLeaves)
        *acceptedLeaves = source.leaves;
    if (!accepted)
        return false;
    DualAttributes reflection{}, texture{};
    reflection.valid = texture.valid = true;
    reflection.stq = reflectionStq;
    texture.stq[2] = {1, 1};
    for (unsigned vertex = 0; vertex < packet.vertices; ++vertex)
    {
        const auto *p = packet.inputs.data() + vertex * 32;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto value = scalar(load(p + axis * 4));
            reflection.position[axis] = vertex ? unite(reflection.position[axis], value) : value;
        }
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            const auto value = scalar(load(p + 12 + axis * 16));
            texture.stq[axis] = vertex ? unite(texture.stq[axis], value) : value;
        }
    }
    texture.position = reflection.position;
    const float opacity = load(packet.constants.data() + 128);
    if (opacity < 0 || opacity > 1)
        return false;
    volatile float scaled = opacity * 128.0f;
    for (auto *attributes : {&reflection, &texture})
    {
        // Conservative RGB ranges prevent assumptions about lighting or the
        // wrapping opacity byte. Exact constant alpha preserves blend selection.
        attributes->colorMin = {0, 0, 0, uint32_t(scaled)};
        attributes->colorMax = {255, 255, 255, uint32_t(scaled)};
    }
    for (unsigned section = 0; section < 4; ++section)
        if (!projectDualBounds(section < 2 ? reflection : texture, packet.constants.data() + (section % 2) * 64, 0,
                               result[section]))
            return false;
    return true;
}
} // namespace tf_gpu
