// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_bounds.h"
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <xmmintrin.h>

namespace tf_gpu
{
namespace
{
constexpr double unit = 0x1p-23;
constexpr double tiny = 0x1p-126;
constexpr double largest = static_cast<double>(std::numeric_limits<float>::max());
constexpr double hostMargin = 32 * std::numeric_limits<double>::epsilon();

struct Environment
{
    fenv_t saved{};
    uint32_t csr = _mm_getcsr();
    bool held = false, ready = false;
    Environment()
    {
        held = std::feholdexcept(&saved) == 0;
        ready = held && std::fesetround(FE_TONEAREST) == 0;
        if (ready)
            _mm_setcsr(_mm_getcsr() & ~0xe040u);
    }
    ~Environment()
    {
        if (held)
            std::fesetenv(&saved);
        _mm_setcsr(csr);
    }
};

uint32_t word(const uint8_t *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}

double normal(uint32_t bits)
{
    if (!(bits & 0x7f800000u))
        bits &= 0x80000000u;
    return static_cast<double>(std::bit_cast<float>(bits));
}

double magnitude(DualRange r)
{
    return std::max(std::abs(r.low), std::abs(r.high));
}

double roundingError(double value)
{
    // A finite RTZ binary32 operation loses less than one ULP, or less than
    // FLT_MIN when the result flushes to zero. The double margin bounds host
    // arithmetic in the error estimate itself, including cancellation.
    return std::max(value * unit, tiny) + value * hostMargin;
}

DualRange expanded(DualRange r, double error)
{
    return {r.low - error, r.high + error};
}

bool finite(DualRange r)
{
    return std::isfinite(r.low) && std::isfinite(r.high) && r.low <= r.high && magnitude(r) < largest;
}

DualRange multiply(DualRange a, DualRange b)
{
    const double products[] = {a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high};
    return {*std::min_element(std::begin(products), std::end(products)),
            *std::max_element(std::begin(products), std::end(products))};
}

struct Affine
{
    DualRange exact;
    double error = 0;
};

bool affine(const DualAttributes &a, const double *matrix, unsigned lane, Affine &out)
{
    out = {{matrix[12 + lane], matrix[12 + lane]}, 0};
    double absoluteTerms = std::abs(matrix[12 + lane]);
    for (int column = 2; column >= 0; --column)
    {
        const double coefficient = matrix[column * 4 + lane];
        const auto product = multiply(a.position[column], {coefficient, coefficient});
        if (!finite(product))
            return false;
        out.error += roundingError(magnitude(product));
        out.exact.low += product.low;
        out.exact.high += product.high;
        absoluteTerms += magnitude(product);
        const double maxIntermediate = magnitude(out.exact) + out.error + absoluteTerms * hostMargin;
        if (maxIntermediate >= largest)
            return false;
        out.error += roundingError(maxIntermediate);
    }
    out.error += absoluteTerms * hostMargin;
    return finite(expanded(out.exact, out.error));
}

bool packBounds(DualRange screen, unsigned axis, DualDrawBounds &out)
{
    const double low = std::floor(screen.low * 16), high = std::ceil(screen.high * 16);
    if (!std::isfinite(low) || !std::isfinite(high) || low < INT32_MIN || high > INT32_MAX)
        return false;
    const int64_t first = static_cast<int64_t>(low), last = static_cast<int64_t>(high);
    const unsigned bits = axis < 2 ? 16 : 28, shift = axis < 2 ? 0 : 4;
    const uint32_t mask = axis < 2 ? 0xffffu : 0xffffffu;
    if ((first >> bits) != (last >> bits))
    {
        out.xyzMin[axis] = 0;
        out.xyzMax[axis] = mask;
        out.wrappedAxes |= 1u << axis;
    }
    else
    {
        out.xyzMin[axis] = (uint32_t(first) >> shift) & mask;
        out.xyzMax[axis] = (uint32_t(last) >> shift) & mask;
    }
    return true;
}
} // namespace

bool summarizeDualInputs(const void *inputs, uint32_t vertices, DualAttributes &result)
{
    result = {};
    if (!inputs || !vertices || vertices > 1024 * 1024)
        return false;
    Environment environment;
    if (!environment.ready)
        return false;
    const auto *source = static_cast<const uint8_t *>(inputs);
    for (uint32_t i = 0; i < vertices; ++i)
    {
        DualAttributes vertex;
        vertex.valid = true;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const double p = normal(word(source + i * 32 + axis * 4));
            const double stq = normal(word(source + i * 32 + 16 + axis * 4));
            if (!std::isfinite(p) || !std::isfinite(stq))
            {
                result = {};
                return false;
            }
            vertex.position[axis] = {p, p};
            vertex.stq[axis] = {stq, stq};
        }
        const uint32_t color = word(source + i * 32 + 28);
        for (unsigned channel = 0; channel < 4; ++channel)
            vertex.colorMin[channel] = vertex.colorMax[channel] = (color >> (channel * 8)) & 255;
        mergeDualAttributes(result, vertex);
    }
    return true;
}

void mergeDualAttributes(DualAttributes &result, const DualAttributes &other)
{
    if (!other.valid)
        return;
    if (!result.valid)
    {
        result = other;
        return;
    }
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        result.position[axis].low = std::min(result.position[axis].low, other.position[axis].low);
        result.position[axis].high = std::max(result.position[axis].high, other.position[axis].high);
        result.stq[axis].low = std::min(result.stq[axis].low, other.stq[axis].low);
        result.stq[axis].high = std::max(result.stq[axis].high, other.stq[axis].high);
    }
    for (unsigned channel = 0; channel < 4; ++channel)
    {
        result.colorMin[channel] = std::min(result.colorMin[channel], other.colorMin[channel]);
        result.colorMax[channel] = std::max(result.colorMax[channel], other.colorMax[channel]);
    }
}

static bool projectDualBoundsImpl(const DualAttributes &a, const void *matrixWords, uint32_t biasWord,
                                  DualDrawBounds &result, const DualAttributes *other, DualDrawBounds *otherResult)
{
    result = {};
    if (!a.valid || !matrixWords)
        return false;
    if (other)
    {
        *otherResult = {};
        if (!other->valid)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (a.position[axis].low != other->position[axis].low ||
                a.position[axis].high != other->position[axis].high)
                return false;
    }
    Environment environment;
    if (!environment.ready)
        return false;
    const auto *words = static_cast<const uint8_t *>(matrixWords);
    double matrix[16];
    for (unsigned i = 0; i < 16; ++i)
    {
        matrix[i] = normal(word(words + i * 4));
        if (!std::isfinite(matrix[i]))
            return false;
    }
    const double bias = normal(biasWord);
    if (!std::isfinite(bias))
        return false;
    Affine homogeneous[4];
    for (unsigned lane = 0; lane < 4; ++lane)
        if (!affine(a, matrix, lane, homogeneous[lane]))
            return false;
    const auto w = expanded(homogeneous[3].exact, homogeneous[3].error);
    if (w.low <= 0)
        return false;
    const double reciprocalError = roundingError(1 / w.low);
    const auto reciprocal = expanded({1 / w.high, 1 / w.low}, reciprocalError);
    if (reciprocal.low <= 0 || !finite(reciprocal))
        return false;

    DualRange screen[3];
    for (auto &axis : screen)
        axis = {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    // A linear-fractional function over a box with positive denominator has
    // extrema at corners. Include independent constant +/- error envelopes
    // for numerator and denominator before applying reciprocal/multiply error.
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        double value[4];
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            value[lane] = matrix[12 + lane];
            for (int column = 2; column >= 0; --column)
                value[lane] += matrix[column * 4 + lane] *
                               ((corner >> column) & 1 ? a.position[column].high : a.position[column].low);
        }
        if (value[3] - homogeneous[3].error <= 0)
            return false;
        const DualRange inverse{1 / (value[3] + homogeneous[3].error), 1 / (value[3] - homogeneous[3].error)};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto ratio = multiply(expanded({value[axis], value[axis]}, homogeneous[axis].error), inverse);
            screen[axis].low = std::min(screen[axis].low, ratio.low);
            screen[axis].high = std::max(screen[axis].high, ratio.high);
        }
    }
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        const double error = magnitude(expanded(homogeneous[axis].exact, homogeneous[axis].error)) * reciprocalError;
        screen[axis] = expanded(screen[axis], error + roundingError(magnitude(screen[axis]) + error));
        if (!packBounds(screen[axis], axis, result))
            return false;
    }
    const auto attributes = [&](const DualAttributes &source, DualDrawBounds &output) {
        DualRange sourceStq[3];
        double outputError[3];
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const DualRange difference{source.stq[axis].low - bias, source.stq[axis].high - bias};
            sourceStq[axis] = expanded(difference, roundingError(magnitude(difference)));
            const auto product = multiply(sourceStq[axis], reciprocal);
            if (!finite(sourceStq[axis]) || !finite(product))
                return false;
            outputError[axis] = roundingError(magnitude(product));
            output.stq[axis] = expanded(product, outputError[axis]);
            if (!finite(output.stq[axis]))
                return false;
        }
        if (sourceStq[2].low <= 0 || output.stq[2].low <= 0)
            return false;
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            const auto ratio = multiply(sourceStq[axis], {1 / sourceStq[2].high, 1 / sourceStq[2].low});
            const double error = (outputError[axis] + magnitude(ratio) * outputError[2]) / output.stq[2].low;
            output.texture[axis] = expanded(ratio, error + 4 * roundingError(magnitude(ratio) + error));
            if (!finite(output.texture[axis]))
                return false;
        }
        output.colorMin = source.colorMin;
        output.colorMax = source.colorMax;
        return true;
    };
    if (other)
    {
        *otherResult = result;
        if (!attributes(*other, *otherResult))
            return false;
    }
    return attributes(a, result);
}
bool projectDualBounds(const DualAttributes &a, const void *matrix, uint32_t bias, DualDrawBounds &result)
{
    return projectDualBoundsImpl(a, matrix, bias, result, nullptr, nullptr);
}
bool projectDualBoundsPair(const DualAttributes &first, const DualAttributes &second, const void *matrix, uint32_t bias,
                           std::array<DualDrawBounds, 2> &result)
{
    result = {};
    return projectDualBoundsImpl(first, matrix, bias, result[0], &second, &result[1]);
}
bool projectGrassBounds(const void *inputs, std::array<DualDrawBounds, 2> &result)
{
    result = {};
    if (!inputs)
        return false;
    Environment environment;
    if (!environment.ready || std::fesetround(FE_TOWARDZERO) != 0)
        return false;
    _mm_setcsr(_mm_getcsr() | 0x8040u); // Match VU input/output denormal flushing.
    const auto *data = static_cast<const uint8_t *>(inputs);
    struct BinaryRange
    {
        float low = 0, high = 0;
    };
    const auto valid = [](BinaryRange value) {
        return std::isfinite(value.low) && std::isfinite(value.high) && value.low <= value.high &&
               std::max(std::abs(value.low), std::abs(value.high)) < std::numeric_limits<float>::max();
    };
    float vectors[16][4];
    for (unsigned vector = 0; vector < 16; ++vector)
        for (unsigned axis = 0; axis < 4; ++axis)
        {
            uint32_t bits = word(data + vector * 16 + axis * 4);
            if (!(bits & 0x7f800000u))
                bits &= 0x80000000u;
            vectors[vector][axis] = std::bit_cast<float>(bits);
            if (!std::isfinite(vectors[vector][axis]))
                return false;
        }
    for (unsigned field = 0; field < 2; ++field)
    {
        const auto *stq = data + 256 + field * 16;
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            const uint32_t value = word(stq + axis * 4);
            if (value != 0 && value != 0x3f000000u && value != 0x3f800000u)
                return false;
        }
        if (word(stq + 8) != 0x3f800000u)
            return false;
    }
    const auto unite = [](BinaryRange a, BinaryRange b) {
        return BinaryRange{std::min(a.low, b.low), std::max(a.high, b.high)};
    };
    // Each endpoint remains binary32 in the strict RTZ/FTZ scope. Packed
    // operations avoid scalar volatile stores and double conversions.
    const auto add = [](BinaryRange a, BinaryRange b) {
        const auto sums = _mm_add_ps(_mm_setr_ps(a.low, a.high, a.low, a.high),
                                    _mm_setr_ps(b.low, b.high, b.low, b.high));
        return BinaryRange{_mm_cvtss_f32(sums),
                           _mm_cvtss_f32(_mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 1, 1, 1)))};
    };
    const auto product = [](BinaryRange a, BinaryRange b) {
        const auto values = _mm_mul_ps(_mm_setr_ps(a.low, a.low, a.high, a.high),
                                      _mm_setr_ps(b.low, b.high, b.low, b.high));
        if (_mm_movemask_ps(_mm_cmpunord_ps(values, values)))
            return BinaryRange{std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
        // Keep the original first-endpoint tie behavior, including signed zero.
        auto low = _mm_min_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(1, 1, 1, 1)), values);
        auto high = _mm_max_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(1, 1, 1, 1)), values);
        low = _mm_min_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(2, 2, 2, 2)), low);
        high = _mm_max_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(2, 2, 2, 2)), high);
        low = _mm_min_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(3, 3, 3, 3)), low);
        high = _mm_max_ss(_mm_shuffle_ps(values, values, _MM_SHUFFLE(3, 3, 3, 3)), high);
        return BinaryRange{_mm_cvtss_f32(low), _mm_cvtss_f32(high)};
    };
    const auto scalar = [&](unsigned vector, unsigned axis) {
        return BinaryRange{vectors[vector][axis], vectors[vector][axis]};
    };
    BinaryRange weights[4];
    for (unsigned axis = 0; axis < 4; ++axis)
    {
        weights[axis] = scalar(12, axis);
        for (unsigned pair = 1; pair < 4; ++pair)
            weights[axis] = unite(weights[axis], scalar(12 + pair, axis));
    }
    const auto interpolate = [&](unsigned first, unsigned axis) {
        const auto base = add(scalar(first, axis), product(scalar(first + 1, axis), weights[0]));
        const auto a = add(base, product(scalar(first + 2, axis), weights[1]));
        const auto b =
            add(add(base, product(scalar(first + 1, axis), weights[2])), product(scalar(first + 2, axis), weights[3]));
        return unite(a, b);
    };
    std::array<uint32_t, 4> colorMin{}, colorMax{};
    for (unsigned channel = 0; channel < 4; ++channel)
    {
        const auto range = interpolate(9, channel);
        if (!valid(range) || range.low < 0)
            return false;
        const uint32_t low = std::bit_cast<uint32_t>(float(range.low));
        const uint32_t high = std::bit_cast<uint32_t>(float(range.high));
        colorMin[channel] = (low >> 8) == (high >> 8) ? low & 255u : 0;
        colorMax[channel] = (low >> 8) == (high >> 8) ? high & 255u : 255;
    }
    for (unsigned field = 0; field < 2; ++field)
    {
        auto &bounds = result[field];
        const auto w = interpolate(3 + field * 3, 3);
        if (!valid(w) || w.low <= 0)
            return false;
        volatile float qLow = 1.0f / float(w.high), qHigh = 1.0f / float(w.low);
        const BinaryRange reciprocal{qLow, qHigh};
        if (!valid(reciprocal) || reciprocal.low <= 0)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto origin = interpolate(3 + field * 3, axis);
            const auto corner = add(origin, scalar(2, axis));
            const auto screen = product(unite(origin, corner), reciprocal);
            if (!valid(origin) || !valid(corner) || !valid(screen) || !packBounds({screen.low, screen.high}, axis, bounds))
                return false;
            bounds.stq[axis] = axis == 2 ? DualRange{1, 1} : DualRange{0, 1};
        }
        bounds.texture = {DualRange{0, 1}, DualRange{0, 1}};
        bounds.colorMin = colorMin;
        bounds.colorMax = colorMax;
    }
    return true;
}
} // namespace tf_gpu
