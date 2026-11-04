// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_grass_geometry.h"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace tf_native {
namespace {
using V = GrassVector;
using Triangle = std::array<V, 3>;
using Fields = std::array<Triangle, 2>;
constexpr uint32_t kMaxDepth = 8, kMaxBatches = 65536;

struct FloatEnvironment {
    fenv_t saved{};
    bool savedOk = false, ready = false;
#if defined(_M_X64) || defined(__x86_64__)
    uint32_t csr = _mm_getcsr();
#endif
    FloatEnvironment() {
        savedOk = std::fegetenv(&saved) == 0;
        ready = savedOk && std::fesetround(FE_TOWARDZERO) == 0;
#if defined(_M_X64) || defined(__x86_64__)
        // VU arithmetic flushes subnormal operands and results to signed zero.
        if (ready) _mm_setcsr(_mm_getcsr() | 0x8040u);
#else
        ready = false; // Non-x86 denormal behavior has not been qualified.
#endif
    }
    ~FloatEnvironment() {
        if (savedOk) std::fesetenv(&saved);
#if defined(_M_X64) || defined(__x86_64__)
        _mm_setcsr(csr);
#endif
    }
};
V add(const V& a, const V& b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2], a[3]+b[3]}; }
V subtract(const V& a, const V& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2], a[3]-b[3]}; }
V multiply(const V& a, const V& b) { return {a[0]*b[0], a[1]*b[1], a[2]*b[2], a[3]*b[3]}; }
V scale(const V& a, float b) { return {a[0]*b, a[1]*b, a[2]*b, a[3]*b}; }
V midpoint(const V& a, const V& b) { return add(scale(a, 0.5f), scale(b, 0.5f)); }
bool finite(const V& a) { return std::all_of(a.begin(), a.end(), [](float value) { return std::isfinite(value); }); }
V transform(const std::array<V, 4>& matrix, const V& point) {
    // Preserve the shader's separate multiply/add order; no fused operations.
    return add(add(add(matrix[3], scale(matrix[2], point[2])), scale(matrix[1], point[1])), scale(matrix[0], point[0]));
}
float areaTwice(const V& a, const V& b, const V& c) {
    const V u = subtract(a, c), v = subtract(b, c);
    const float x = u[1]*v[2] - u[2]*v[1];
    const float y = u[2]*v[0] - u[0]*v[2];
    const float z = u[0]*v[1] - u[1]*v[0];
    return std::sqrt((x*x + y*y) + z*z);
}
uint32_t clip(const V& position) {
    const float w = std::fabs(position[3]);
    uint32_t flags = 0;
    for (unsigned lane = 0; lane < 3; ++lane) {
        if (position[lane] > w) flags |= 1u << (lane*2);
        if (position[lane] < -w) flags |= 2u << (lane*2);
    }
    return flags;
}
uint32_t fixed4(float value) {
    value *= 16.0f;
    if (value >= 2147483648.0f) return 0x7fffffffu;
    if (value <= -2147483648.0f) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}
float reciprocal(float value) {
    if (value == 0.0f) return std::copysign(std::numeric_limits<float>::max(), value);
    return 1.0f / value;
}
GrassWords raw(const V& value) {
    return {std::bit_cast<uint32_t>(value[0]), std::bit_cast<uint32_t>(value[1]),
            std::bit_cast<uint32_t>(value[2]), std::bit_cast<uint32_t>(value[3])};
}

struct Builder {
    const GrassContext& context;
    std::vector<GrassBatch>& output;
    GrassStats& stats;
    V clipPosition(const V& point) const {
        return add(scale(context.clipOffset, point[3]), multiply(context.clipScale, point));
    }
    bool outside(const Fields& triangle) const {
        uint32_t flags = 63;
        for (const auto& field : triangle)
            for (const auto& point : field) flags &= clip(clipPosition(point));
        return flags != 0;
    }
    bool emit(const Fields& triangle, const Triangle& colors, unsigned group, float fraction) {
        if (group >= 8 || output.size() >= kMaxBatches) return false;
        V offset = context.billboardOffset;
        offset[1] *= fraction;
        std::array<V, 2> a, b;
        for (unsigned field = 0; field < 2; ++field) {
            a[field] = subtract(triangle[field][0], triangle[field][2]);
            b[field] = subtract(triangle[field][1], triangle[field][2]);
        }
        const V colorA = subtract(colors[0], colors[2]), colorB = subtract(colors[1], colors[2]);
        V bias = add(colors[2], V{8388608.0f, 8388608.0f, 8388608.0f, 0.0f});
        bias[3] = 8388735.0f;
        GrassBatch batch;
        for (unsigned pair = 0; pair < 4; ++pair) {
            const V& weights = context.scatter[group*4 + pair];
            const V accumColor = add(bias, scale(colorA, weights[0]));
            batch.colors[pair*2] = raw(add(accumColor, scale(colorB, weights[1])));
            batch.colors[pair*2+1] = raw(add(add(accumColor, scale(colorA, weights[2])), scale(colorB, weights[3])));
            for (unsigned field = 0; field < 2; ++field) {
                const V accum = add(triangle[field][2], scale(a[field], weights[0]));
                const std::array<V, 2> points = {add(accum, scale(b[field], weights[1])),
                    add(add(accum, scale(a[field], weights[2])), scale(b[field], weights[3]))};
                for (unsigned second = 0; second < 2; ++second) {
                    const V& point = points[second];
                    const V end = add(point, offset);
                    const float q = reciprocal(point[3]);
                    uint32_t flags = 0x7fff + (clip(clipPosition(point)) & clip(clipPosition(end)));
                    if (flags & 0x8000) flags |= 0xffff0000u;
                    const V first = scale(point, q), last = scale(end, q);
                    if (!finite(first) || !finite(last)) return false;
                    const GrassWords firstWords = {fixed4(first[0]), fixed4(first[1]), fixed4(first[2]), flags};
                    const GrassWords lastWords = {fixed4(last[0]), fixed4(last[1]), fixed4(last[2]), flags};
                    const unsigned index = pair*4 + second*2;
                    batch.positions[field][index] = second ? firstWords : lastWords;
                    batch.positions[field][index+1] = second ? lastWords : firstWords;
                }
            }
        }
        output.push_back(batch);
        return true;
    }
    bool triangle(const Fields& points, const Triangle& colors, float area, float bias, uint32_t depth) {
        if (depth > kMaxDepth || !std::isfinite(area)) return false;
        stats.maxDepth = std::max(stats.maxDepth, depth);
        if (outside(points)) return true;
        float excess = 0.0f;
        for (unsigned side = 0; side < 3; ++side) {
            Fields current;
            Triangle color;
            for (unsigned corner = 0; corner < 3; ++corner) {
                const unsigned source = (side + corner) % 3;
                color[corner] = colors[source];
                for (unsigned field = 0; field < 2; ++field) current[field][corner] = points[field][source];
            }
            const float density = (context.density * color[2][3]) * area;
            if (!std::isfinite(density) || density >= 2147483648.0f || density < 0.0f) return false;
            const float numerator = static_cast<float>(static_cast<int32_t>(density));
            const float distance = std::max(current[0][2][3] - context.depthBias, 0.0f);
            const float denominator = distance * context.distanceScale + 1.0f;
            const float amount = numerator / denominator - bias;
            if (!std::isfinite(amount)) return false;
            const float limited = std::min(amount, 8.0f);
            const int full = static_cast<int>(limited);
            excess = std::max(excess, amount - limited);
            for (int group = 0; group < full; ++group)
                if (!emit(current, color, static_cast<unsigned>(group), 1.0f)) return false;
            const float fraction = amount <= 8.0f ? amount - static_cast<float>(static_cast<int>(amount)) : 0.0f;
            if (fraction > 0.1f && !emit(current, color, static_cast<unsigned>(std::max(full, 0)), fraction)) return false;
        }
        if (excess > 0.0f) {
            ++stats.subdivisions;
            std::array<V, 2> ab, ac, bc;
            for (unsigned field = 0; field < 2; ++field) {
                ab[field] = midpoint(points[field][0], points[field][1]);
                ac[field] = midpoint(points[field][0], points[field][2]);
                bc[field] = midpoint(points[field][1], points[field][2]);
            }
            const V colorAB = midpoint(colors[0], colors[1]), colorAC = midpoint(colors[0], colors[2]);
            const V colorBC = midpoint(colors[1], colors[2]);
            std::array<Fields, 4> children;
            for (unsigned field = 0; field < 2; ++field) {
                children[0][field] = {bc[field], ab[field], ac[field]};
                children[1][field] = {points[field][0], ab[field], ac[field]};
                children[2][field] = {bc[field], ab[field], points[field][1]};
                children[3][field] = {bc[field], points[field][2], ac[field]};
            }
            const std::array<Triangle, 4> childColors = {{{colorBC, colorAB, colorAC},
                {colors[0], colorAB, colorAC}, {colorBC, colorAB, colors[1]}, {colorBC, colors[2], colorAC}}};
            for (unsigned child = 0; child < 4; ++child)
                if (!triangle(children[child], childColors[child], area*0.25f, 2.0f, depth+1)) return false;
        }
        return true;
    }
};
}

bool buildGrassGeometry(const GrassContext& context, const GrassStrip& strip,
                        std::vector<GrassBatch>& output, GrassStats& stats) {
    output.clear(); stats = {};
    const size_t count = strip.vertices.size();
    if (count < 2 || count > 28 || !finite(context.clipScale) || !finite(context.clipOffset) ||
        !finite(context.billboardOffset) || context.billboardOffset[3] != 0.0f ||
        !std::isfinite(context.depthBias) || !std::isfinite(context.density) || context.density < 0.0f ||
        !std::isfinite(context.distanceScale) || context.distanceScale < 0.0f) return false;
    for (const auto& matrix : context.transform) for (const auto& column : matrix) if (!finite(column)) return false;
    for (const auto& weights : context.scatter) if (!finite(weights)) return false;
    for (const auto& vertex : strip.vertices) {
        if (!finite(vertex.position)) return false;
        for (uint32_t channel : vertex.color) if (channel > 255) return false;
    }
    FloatEnvironment environment;
    if (!environment.ready) return false;
    std::array<std::array<V, 28>, 2> transformed;
    std::array<V, 28> colors;
    for (size_t index = 0; index < count; ++index) {
        const auto& vertex = strip.vertices[index];
        for (unsigned field = 0; field < 2; ++field) {
            transformed[field][index] = transform(context.transform[field], vertex.position);
            if (!finite(transformed[field][index])) return false;
        }
        for (unsigned channel = 0; channel < 3; ++channel) colors[index][channel] = static_cast<float>(vertex.color[channel]);
        colors[index][3] = static_cast<float>(vertex.color[3] & 127) * std::bit_cast<float>(0x3c010204u);
    }
    Builder builder{context, output, stats};
    for (size_t index = 2; index < count; ++index) {
        ++stats.triangles;
        Fields triangle;
        for (unsigned field = 0; field < 2; ++field)
            triangle[field] = {transformed[field][index-2], transformed[field][index-1], transformed[field][index]};
        const unsigned shift = index < 16 ? 15u-static_cast<unsigned>(index) : 31u-static_cast<unsigned>(index);
        const bool restart = (strip.restartMasks[index < 16 ? 0 : 1] & (1u << shift)) != 0;
        if (restart || builder.outside(triangle)) { ++stats.coarseCulled; continue; }
        const float area = areaTwice(strip.vertices[index-2].position, strip.vertices[index-1].position, strip.vertices[index].position);
        if (!builder.triangle(triangle, {colors[index-2], colors[index-1], colors[index]}, area, 0.0f, 0)) {
            output.clear(); return false;
        }
    }
    return true;
}
}
