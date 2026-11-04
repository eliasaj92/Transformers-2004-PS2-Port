// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gs_native_geometry.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <emmintrin.h>

namespace tf_gpu_grass
{
inline bool enabled = false;
using Input = std::array<uint8_t, 256>;
struct Stats
{
    uint64_t selected = 0, attached = 0, declined = 0, overlapping = 0;
    uint64_t recorded = 0, overwrittenQwords = 0, materializedSprites = 0, materializedQwords = 0;
    unsigned dirtyQwords = 0;
};
void install(uint32_t crc, bool acceleratedGpuBypass);
void beginCall(const void *owner, const uint8_t *code, uint8_t *data, uint64_t generation);
void endCall(bool transferred, bool fellBack);
bool active();
bool beginEmitter(uint32_t pc, uint8_t *data, const Input &input, bool overlapping);
void completeEmitter(uint8_t *data, unsigned base);
void ordinary();
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes);
Stats stats();
void finish();

template <class VU> Input inputs(const VU &v, const uint8_t *data)
{
    Input input;
    constexpr unsigned registers[] = {1, 2, 3, 9, 16, 17, 10, 18, 19, 22, 20, 21};
    for (unsigned i = 0; i < 12; ++i)
        std::memcpy(input.data() + i * 16, v.m_state.vf[registers[i]], 16);
    for (unsigned i = 0; i < 4; ++i)
        std::memcpy(input.data() + 192 + i * 16, data + ((uint16_t(v.m_state.vi[5]) + i - 1u) & 1023u) * 16, 16);
    return input;
}
// The final accelerated iteration overwrites all retained results except the
// previous pair's second-field clipping flags. Reconstruct only those flags;
// the GPU performs the skipped coordinate, reciprocal and color calculations.
template <class VU> void skipMiddlePairs(VU &v, const uint8_t *data)
{
    const auto add = [](__m128 x, __m128 y) { return _mm_add_ps(x, y); };
    const auto scale = [](__m128 x, float y) { return _mm_mul_ps(x, _mm_set1_ps(y)); };
    float weights[4];
    std::memcpy(weights, data + ((uint16_t(v.m_state.vi[5]) + 1u) & 1023u) * 16u, 16);
    const __m128 edge = _mm_loadu_ps(v.m_state.vf[18]);
    __m128 point = add(_mm_loadu_ps(v.m_state.vf[10]), scale(edge, weights[0]));
    point = add(add(point, scale(edge, weights[2])), scale(_mm_loadu_ps(v.m_state.vf[19]), weights[3]));
    const auto flags = [&](const __m128 p) {
        const __m128 clip = add(_mm_mul_ps(_mm_loadu_ps(v.m_state.vf[2]), _mm_shuffle_ps(p, p, 255)),
                                _mm_mul_ps(_mm_loadu_ps(v.m_state.vf[1]), p));
        float f[4];
        _mm_storeu_ps(f, clip);
        uint32_t bits = 0;
        for (unsigned i = 0; i < 3; ++i)
        {
            if (f[i] > std::fabs(f[3]))
                bits |= 1u << (i * 2);
            if (f[i] < -std::fabs(f[3]))
                bits |= 2u << (i * 2);
        }
        return bits;
    };
    const uint32_t retained = flags(point) & flags(add(point, _mm_loadu_ps(v.m_state.vf[3])));
    // Advances the real pending transfer and pipeline calendar, not just the
    // cycle counter. The following accelerated iteration retains normal timing.
    v.advanceTo(v.m_cycle + 118);
    v.m_state.vi[10] = int16_t(retained);
    v.m_state.vi[12] = 63;
    v.m_state.vi[5] = int16_t(v.m_state.vi[5] + 2);
    v.m_state.vi[8] = int16_t(v.m_state.vi[8] + 16);
    v.m_state.vi[9] = 2;
    v.m_state.branchTarget = v.m_state.pc;
    v.m_state.branchDelay = 0;
}
template <class VU, class Body> bool loop(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, Body body)
{
    if (!active())
        return body(v, data, size, end, ended);
    // Leave budget for the retained iteration, epilogue and kick as well as
    // the skipped cycles. A declined loop resolves any earlier VU output.
    if (size != 16384 || v.m_state.vi[9] != 6 || end < v.m_cycle || end - v.m_cycle < 204 || v.m_state.branchPending ||
        v.m_state.ebit || v.m_state.haltAfterDelaySlot || v.m_stopRequested || v.m_state.dBitEnabled ||
        v.m_state.tBitEnabled || (v.m_state.status & 0x3c0) != 0x3c0 ||
        !beginEmitter(v.m_state.pc, data, inputs(v, data), v.m_xgkick.active))
    {
        ordinary();
        return body(v, data, size, end, ended);
    }
    skipMiddlePairs(v, data);
    if (!body(v, data, size, end, ended))
        throw std::runtime_error("Qualified grass loop changed accelerated control flow");
    return true;
}
template <class VU, class Body> bool epilogue(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, Body body)
{
    if (!body(v, data, size, end, ended))
        return false;
    if (active())
        completeEmitter(data, (uint16_t(v.m_state.vi[2]) - 6u) & 1023u);
    return true;
}
} // namespace tf_gpu_grass
