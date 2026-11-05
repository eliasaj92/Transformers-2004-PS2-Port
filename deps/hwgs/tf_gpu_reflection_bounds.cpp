// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_bounds.h"
#include "runtime/gs/gs_native_reflection_math.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>
#include <limits>
#include <xmmintrin.h>

namespace tf_gpu
{
namespace
{
// Every lighting interval endpoint is a binary32 result. Keeping it binary32
// avoids repeated double/float conversions in the recursive source bounds.
struct Range
{
    float low = 0, high = 0;
};
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
bool finite(Range a)
{
    return std::isfinite(a.low) && std::isfinite(a.high) && a.low <= a.high;
}
// Endpoints are binary32 values. Each monotone operation uses the VU's
// RTZ/FTZ environment; /fp:strict prevents contraction and constant folding
// across the independently rounded multiply/add/sqrt/reciprocal operations.
Range plus(Range a, Range b)
{
    const __m128 values =
        _mm_add_ps(_mm_setr_ps(a.low, a.high, a.low, a.high), _mm_setr_ps(b.low, b.high, b.low, b.high));
    return {_mm_cvtss_f32(values), _mm_cvtss_f32(_mm_shuffle_ps(values, values, _MM_SHUFFLE(1, 1, 1, 1)))};
}
Range times(Range a, Range b)
{
    const __m128 products =
        _mm_mul_ps(_mm_setr_ps(a.low, a.low, a.high, a.high), _mm_setr_ps(b.low, b.high, b.low, b.high));
    // MINPS can hide a NaN depending on operand order. Decline that domain
    // explicitly instead of turning an invalid interval into a finite bound.
    if (_mm_movemask_ps(_mm_cmpunord_ps(products, products)))
        return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
    const __m128 opposite = _mm_movehl_ps(products, products);
    const __m128 low = _mm_min_ps(products, opposite), high = _mm_max_ps(products, opposite);
    return {_mm_cvtss_f32(_mm_min_ss(low, _mm_shuffle_ps(low, low, _MM_SHUFFLE(1, 1, 1, 1)))),
            _mm_cvtss_f32(_mm_max_ss(high, _mm_shuffle_ps(high, high, _MM_SHUFFLE(1, 1, 1, 1))))};
}

// Three independent axes share SIMD instructions; the fourth lane is unused.
// Preserve the original endpoint and x+y+z reduction order in strict RTZ/FTZ.
#if defined(_MSC_VER)
#define TF_BOUNDS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define TF_BOUNDS_INLINE inline __attribute__((always_inline))
#else
#define TF_BOUNDS_INLINE inline
#endif
struct PackedRange3
{
    __m128 low, high;
};
TF_BOUNDS_INLINE PackedRange3 packRanges(const Vector &v)
{
    return {_mm_setr_ps(v[0].low, v[1].low, v[2].low, 0),
            _mm_setr_ps(v[0].high, v[1].high, v[2].high, 0)};
}
TF_BOUNDS_INLINE PackedRange3 pointRanges(const uint8_t *p)
{
    const auto v = _mm_setr_ps(load(p), load(p + 4), load(p + 8), 0);
    return {v, v};
}
TF_BOUNDS_INLINE PackedRange3 splatRange(Range r)
{
    return {_mm_set1_ps(r.low), _mm_set1_ps(r.high)};
}
TF_BOUNDS_INLINE PackedRange3 addRanges(PackedRange3 a, PackedRange3 b)
{
    return {_mm_add_ps(a.low, b.low), _mm_add_ps(a.high, b.high)};
}
TF_BOUNDS_INLINE PackedRange3 negateRanges(PackedRange3 a)
{
    const auto sign = _mm_set1_ps(-0.0f);
    return {_mm_xor_ps(a.high, sign), _mm_xor_ps(a.low, sign)};
}
TF_BOUNDS_INLINE PackedRange3 multiplyRanges(PackedRange3 a, PackedRange3 b)
{
    const auto p0 = _mm_mul_ps(a.low, b.low), p1 = _mm_mul_ps(a.low, b.high);
    const auto p2 = _mm_mul_ps(a.high, b.low), p3 = _mm_mul_ps(a.high, b.high);
    const auto unordered = _mm_or_ps(_mm_or_ps(_mm_cmpunord_ps(p0, p0), _mm_cmpunord_ps(p1, p1)),
                                     _mm_or_ps(_mm_cmpunord_ps(p2, p2), _mm_cmpunord_ps(p3, p3)));
    if (_mm_movemask_ps(unordered) & 7)
    {
        const auto invalid = _mm_set1_ps(std::numeric_limits<float>::quiet_NaN());
        return {invalid, invalid};
    }
    // Match the existing endpoint reduction order, including zero ties.
    return {_mm_min_ps(_mm_min_ps(p0, p2), _mm_min_ps(p1, p3)),
            _mm_max_ps(_mm_max_ps(p0, p2), _mm_max_ps(p1, p3))};
}
TF_BOUNDS_INLINE PackedRange3 scaleRanges(PackedRange3 a, Range b)
{
    return multiplyRanges(a, splatRange(b));
}
TF_BOUNDS_INLINE float sumLanes(__m128 value)
{
    const auto xy = _mm_add_ss(value, _mm_shuffle_ps(value, value, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(_mm_add_ss(xy, _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2))));
}
TF_BOUNDS_INLINE Range dotRanges(PackedRange3 a, PackedRange3 b)
{
    const auto product = multiplyRanges(a, b);
    return {sumLanes(product.low), sumLanes(product.high)};
}
template <unsigned Axis> TF_BOUNDS_INLINE Range laneRange(PackedRange3 a)
{
    return {_mm_cvtss_f32(_mm_shuffle_ps(a.low, a.low, _MM_SHUFFLE(Axis, Axis, Axis, Axis))),
            _mm_cvtss_f32(_mm_shuffle_ps(a.high, a.high, _MM_SHUFFLE(Axis, Axis, Axis, Axis)))};
}
TF_BOUNDS_INLINE bool normalizeRanges(PackedRange3 a, PackedRange3 &result)
{
    const auto absLow = _mm_andnot_ps(_mm_set1_ps(-0.0f), a.low);
    const auto absHigh = _mm_andnot_ps(_mm_set1_ps(-0.0f), a.high);
    const auto maximum = _mm_set1_ps(std::numeric_limits<float>::max());
    const auto valid = _mm_and_ps(_mm_cmple_ps(a.low, a.high),
                                 _mm_and_ps(_mm_cmple_ps(absLow, maximum), _mm_cmple_ps(absHigh, maximum)));
    if ((_mm_movemask_ps(valid) & 7) != 7)
        return false;
    const auto zero = _mm_setzero_ps();
    const auto crosses = _mm_and_ps(_mm_cmple_ps(a.low, zero), _mm_cmpge_ps(a.high, zero));
    const auto low = _mm_andnot_ps(crosses, _mm_min_ps(absLow, absHigh));
    const auto high = _mm_max_ps(absLow, absHigh);
    const Range length{sumLanes(_mm_mul_ps(low, low)), sumLanes(_mm_mul_ps(high, high))};
    if (!finite(length) || length.low <= 0)
        return false;
    const auto roots = _mm_sqrt_ps(_mm_setr_ps(length.high, length.low, length.high, length.low));
    const auto inverse = _mm_div_ps(_mm_set1_ps(1.0f), roots);
    const Range q{_mm_cvtss_f32(inverse),
                  _mm_cvtss_f32(_mm_shuffle_ps(inverse, inverse, _MM_SHUFFLE(1, 1, 1, 1)))};
    if (!finite(q))
        return false;
    result = scaleRanges(a, q);
    return true;
}

#undef TF_BOUNDS_INLINE

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
            const auto pos = packRanges(position), nm = packRanges(normal);
            const auto eye = addRanges(pointRanges(constants + 64), negateRanges(pos));
            PackedRange3 view, direction;
            if (!normalizeRanges(eye, view))
                return false;
            const auto facing = dotRanges(nm, view);
            const auto reflected = addRanges(scaleRanges(addRanges(nm, nm), facing), negateRanges(view));
            const auto transformed = addRanges(
                addRanges(scaleRanges(pointRanges(constants), laneRange<0>(reflected)),
                        scaleRanges(pointRanges(constants + 16), laneRange<1>(reflected))),
                scaleRanges(pointRanges(constants + 32), laneRange<2>(reflected)));
            if (!normalizeRanges(transformed, direction))
                return false;
            stq[2] = plus(laneRange<2>(direction), scalar(1.0f));
            if (!finite(stq[2]) || stq[2].low <= 0)
                return false;
            volatile float low = 1.0f / float(stq[2].high), high = 1.0f / float(stq[2].low);
            const auto q = times(scalar(0.5f), {low, high});
            const auto uv = addRanges(scaleRanges(direction, q), splatRange(scalar(0.5f)));
            stq[0] = laneRange<0>(uv);
            stq[1] = laneRange<1>(uv);
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
            if (double(normal[axis].high) - normal[axis].low > spread)
            {
                spread = double(normal[axis].high) - normal[axis].low;
                lane = 4 + axis;
            }
        if (spread == 0)
            for (unsigned axis = 0; axis < 3; ++axis)
                if (double(position[axis].high) - position[axis].low > spread)
                {
                    spread = double(position[axis].high) - position[axis].low;
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
    for (unsigned axis = 0; axis < 3; ++axis)
        reflection.stq[axis] = {reflectionStq[axis].low, reflectionStq[axis].high};
    texture.stq[2] = {1, 1};
    for (unsigned vertex = 0; vertex < packet.vertices; ++vertex)
    {
        const auto *p = packet.inputs.data() + vertex * 32;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const double value = load(p + axis * 4);
            reflection.position[axis] = vertex ? DualRange{std::min(reflection.position[axis].low, value),
                                                           std::max(reflection.position[axis].high, value)}
                                               : DualRange{value, value};
        }
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            const double value = load(p + 12 + axis * 16);
            texture.stq[axis] =
                vertex ? DualRange{std::min(texture.stq[axis].low, value), std::max(texture.stq[axis].high, value)}
                       : DualRange{value, value};
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
    for (unsigned field = 0; field < 2; ++field)
    {
        std::array<DualDrawBounds, 2> pair;
        if (!projectDualBoundsPair(reflection, texture, packet.constants.data() + field * 64, 0, pair))
            return false;
        result[field] = pair[0];
        result[field + 2] = pair[1];
    }
    return true;
}
} // namespace tf_gpu
