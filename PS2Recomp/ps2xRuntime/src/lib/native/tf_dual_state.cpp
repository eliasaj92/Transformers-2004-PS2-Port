// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_dual_state.h"
#include "runtime/ps2_vu1.h"
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace tf_native
{
namespace
{
struct FloatEnvironment
{
    fenv_t saved{};
    bool savedOk = false, ready = false;
#if defined(_M_X64) || defined(__x86_64__)
    uint32_t csr = _mm_getcsr();
#endif
    FloatEnvironment()
    {
        savedOk = std::fegetenv(&saved) == 0;
        ready = savedOk && std::fesetround(FE_TOWARDZERO) == 0;
#if defined(_M_X64) || defined(__x86_64__)
        if (ready)
            _mm_setcsr(_mm_getcsr() | 0x8040u);
#else
        ready = false;
#endif
    }
    ~FloatEnvironment()
    {
        if (savedOk)
            std::fesetenv(&saved);
#if defined(_M_X64) || defined(__x86_64__)
        _mm_setcsr(csr);
#endif
    }
};

uint32_t word(const uint8_t *data, unsigned qword, unsigned lane)
{
    uint32_t value;
    std::memcpy(&value, data + qword * 16 + lane * 4, 4);
    return value;
}
void store(uint8_t *data, unsigned qword, unsigned lane, uint32_t value)
{
    std::memcpy(data + qword * 16 + lane * 4, &value, 4);
}
float normal(uint32_t bits)
{
    const uint32_t exponent = bits & 0x7f800000u;
    if (!exponent)
        bits &= 0x80000000u;
    else if (exponent == 0x7f800000u)
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    return std::bit_cast<float>(bits);
}
uint32_t subtract(uint32_t left, uint32_t right)
{
    const float value = normal(left) - normal(right);
    return std::bit_cast<uint32_t>(normal(std::bit_cast<uint32_t>(value)));
}
uint32_t restart(const uint32_t *masks, unsigned vertex)
{
    const uint16_t bits = static_cast<uint16_t>(masks[vertex / 16] << (vertex % 16));
    return bits & 0x8000u ? uint32_t(bits) | 0xffff0000u : bits;
}
uint32_t fixed4(float value)
{
    value *= 16.0f;
    if (value >= 2147483648.0f)
        return 0x7fffffffu;
    if (value <= -2147483648.0f)
        return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}
} // namespace

bool preserveUnclippedDualState(uint8_t *data, const VU1State &state, uint32_t itop, uint32_t context, uint32_t entry,
                                DualPreservation &effects)
{
    effects = {};
    if (!data || itop < 3 || itop >= 1024 || context > 1009 || (entry != 0x10 && entry != 0xf8))
        return false;
    const uint32_t descriptor = word(data, itop, 0), count = descriptor & 0x7fffu;
    const unsigned dataEnd = itop + count * 6 + 5;
    if (count < 3 || count > 28 || dataEnd > 1023 || word(data, itop, 1) != 3 ||
        (context < dataEnd && context + 15 > itop - 3))
        return false;
    for (unsigned lane = 0; lane < 4; ++lane)
        if (word(data, 21, lane))
            return false;
    const unsigned first = itop + 1, second = first + count * 3 + 4;
    const uint32_t bias = word(data, itop, 2);
    const uint32_t masks[] = {word(data, second - 1, 0), word(data, second - 1, 1)};
    for (unsigned column = 1; column <= 4; ++column)
        for (float value : state.vf[column])
            if (!std::isfinite(value))
                return false;
    for (unsigned vertex = 0; vertex < count; ++vertex)
        for (unsigned lane = 0; lane < 3; ++lane)
            if (!std::isfinite(std::bit_cast<float>(word(data, first + vertex * 3 + 2, lane))))
                return false;
    FloatEnvironment floating;
    if (!floating.ready)
        return false;

    // The 16-vertex inner loop leaves these two registers observable by later
    // clipped draws. A trailing one- or two-vertex block has a different tail.
    const unsigned retained = count == 17 || count == 18 ? 14 : count - 2;
    const unsigned source18 = count == 17 || count == 18 ? 15 : count - 1;
    std::memcpy(effects.vf18.data(), state.vf[18], 16);
    std::memcpy(effects.vf18.data(), data + (first + source18 * 3 + 2) * 16, 12);
    effects.vf17[3] = restart(masks, retained);
    if (count == 18)
    {
        std::memcpy(effects.vf17.data(), data + (first + 17 * 3 + 2) * 16, 12);
    }
    else
    {
        float p[3], projected[4];
        const unsigned projectedVertex = count == 17 ? 16 : retained;
        std::memcpy(p, data + (first + projectedVertex * 3 + 2) * 16, 12);
        for (unsigned lane = 0; lane < 4; ++lane)
        {
            float value = state.vf[4][lane];
            value = value + state.vf[3][lane] * p[2];
            value = value + state.vf[2][lane] * p[1];
            projected[lane] = value + state.vf[1][lane] * p[0];
            if (!std::isfinite(projected[lane]))
                return false;
        }
        if (projected[3] == 0.0f)
            return false;
        const float q = 1.0f / projected[3];
        for (unsigned lane = 0; lane < 3; ++lane)
        {
            const float value =
                count == 17 ? normal(subtract(word(data, first + 16 * 3, lane), bias)) * q : projected[lane] * q;
            if (!std::isfinite(value))
                return false;
            effects.vf17[lane] = count == 17 ? std::bit_cast<uint32_t>(value) : fixed4(value);
        }
    }

    // Manager format 3 de-biases four STQ records at a time, including padding.
    // Its saved descriptor must survive the final partial group.
    uint8_t savedDescriptor[16];
    std::memcpy(savedDescriptor, data + (second - 1) * 16, 16);
    const unsigned padded = (count + 3) & ~3u;
    for (unsigned vertex = 0; vertex < padded; ++vertex)
    {
        const unsigned qword = first + vertex * 3;
        const unsigned firstLane = vertex < count ? 3 : 0;
        for (unsigned lane = firstLane; lane < 4; ++lane)
            store(data, qword, lane, subtract(word(data, qword, lane), bias));
    }
    std::memcpy(data + (second - 1) * 16, savedDescriptor, 16);
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        const uint32_t value = lane ? word(data, 1023, lane) : count;
        store(data, first - 1, lane, value);
        store(data, second - 1, lane, lane ? value : descriptor);
    }
    std::memcpy(data + (first - 4) * 16, data + (context + 8) * 16, 48);
    std::memcpy(data + (second - 4) * 16, data + (context + 12) * 16, 48);
    for (unsigned vertex = 0; vertex < count; ++vertex)
    {
        const unsigned source = first + vertex * 3, target = second + vertex * 3;
        std::memcpy(data + (target + 1) * 16, data + (source + 1) * 16, 16);
        store(data, source + 2, 3, restart(masks, vertex));
        store(data, target + 2, 3, restart(masks, vertex));
        for (unsigned base : {source, target})
        {
            effects.geometryQwords[effects.geometryQwordCount++] = static_cast<uint16_t>(base);
            effects.geometryQwords[effects.geometryQwordCount++] = static_cast<uint16_t>(base + 2);
        }
    }
    effects.guestCycles =
        108 + 28 * count + 11 * (padded / 4) + (count > 16 ? 22 : 0) + (count == 17 ? 1 : 0) + (entry == 0xf8 ? 2 : 0);
    return true;
}

bool prepareUnclippedDualTail(const uint8_t *data, const VU1State &state, uint32_t itop, uint32_t context,
                              uint32_t entry, DualTail &tail)
{
    if (!data || itop >= 1024)
        return false;
    std::memcpy(tail.preserved.data(), data, 16384);
    if (!preserveUnclippedDualState(tail.preserved.data(), state, itop, context, entry, tail.effects))
        return false;
    tail.vertices = word(data, itop, 0) & 0x7fffu;
    const unsigned n = tail.vertices;
    // The one/two-vertex second block has different retained registers. Keep
    // those uncommon counts complete until their shorter tail is qualified.
    tail.tailVertices = n == 17 || n == 18 ? n : (n & 1u ? 3 : 4);
    const unsigned skipped = n - tail.tailVertices;
    tail.itop = itop;
    tail.tailItop = itop + 3 * skipped;
    const unsigned maskQword = itop + 3 * n + 4;
    std::memcpy(tail.scratch.data(), data, 16384);
    std::memcpy(tail.scratch.data() + tail.tailItop * 16, data + itop * 16, 16);
    store(tail.scratch.data(), tail.tailItop, 0, (word(data, itop, 0) & 0xffff8000u) | tail.tailVertices);
    const uint32_t masks = (word(data, maskQword, 0) << 16) | (word(data, maskQword, 1) & 0xffffu);
    store(tail.scratch.data(), maskQword, 0, (masks << skipped) >> 16);
    std::memcpy(tail.managerRegisters[3].data(), data + maskQword * 16, 16);
    tail.alpha = word(data, itop + 2 + (n > 16 ? 16 : 0) * 3, 3);
    FloatEnvironment floating;
    if (!floating.ready)
        return false;
    for (unsigned reg = 0; reg < 3; ++reg)
    {
        const unsigned vertex = ((n + 3) & ~3u) - 3 + reg;
        for (unsigned lane = 0; lane < 4; ++lane)
            tail.managerRegisters[reg][lane] = subtract(word(data, itop + 1 + 3 * vertex, lane), word(data, itop, 2));
    }
    return true;
}

void finishUnclippedDualTail(uint8_t *data, const VU1State &before, VU1State &after, const DualTail &tail)
{
    after.cycles = before.cycles + tail.effects.guestCycles;
    after.itop = tail.itop;
    after.vi[1] = tail.itop;
    after.vi[3] = 3 * tail.vertices + 4;
    after.vi[4] = tail.itop + 6 * tail.vertices + 5;
    after.vi[8] = tail.vertices > 16 ? 0 : int(tail.vertices) - 16;
    after.vi[10] = tail.vertices;
    std::memcpy(after.vf[17], tail.effects.vf17.data(), 16);
    std::memcpy(after.vf[18], tail.effects.vf18.data(), 16);
    for (unsigned reg = 0; reg < 4; ++reg)
        std::memcpy(after.vf[22 + reg], tail.managerRegisters[reg].data(), 16);
    std::memcpy(&after.vf[15][3], &tail.alpha, 4);
    std::memcpy(data, tail.preserved.data(), 16384);
    // The short program's second field starts at the full packet's second
    // field, so relocate its resolved tail there. Only xyz lanes are geometric.
    const unsigned skipped = tail.vertices - tail.tailVertices;
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned vertex = 0; vertex < tail.tailVertices; ++vertex)
        {
            const unsigned source = tail.tailItop + 1 + field * (3 * tail.tailVertices + 4) + vertex * 3;
            const unsigned target = tail.itop + 1 + field * (3 * tail.vertices + 4) + (skipped + vertex) * 3;
            std::memcpy(data + target * 16, tail.scratch.data() + source * 16, 12);
            std::memcpy(data + (target + 2) * 16, tail.scratch.data() + (source + 2) * 16, 12);
        }
}
} // namespace tf_native
