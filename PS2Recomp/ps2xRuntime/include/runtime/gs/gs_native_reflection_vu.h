// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gs_native_reflection_math.h"
#include <cstring>
#include <stdexcept>

namespace tf_gpu_reflection
{
inline bool enabled = false;
inline bool poisonForTest = false;
using ResolveMemory = void (*)(const void *, uint8_t *, unsigned, unsigned);
struct Stats
{
    uint64_t selected = 0, attached = 0, declined = 0, lightingSkipped = 0, projectionSkipped = 0;
    uint64_t records = 0, overwritten = 0, preservedW = 0, litVertices = 0, projectedVertices = 0;
    unsigned dirtyQwords = 0;
};
void install(uint32_t crc, bool acceleratedGpuBypass);
bool beginCall(uint32_t pc, bool continuing, unsigned itop, uint64_t generation, const void *owner, const uint8_t *code,
               uint8_t *data, ResolveMemory resolve, ResolveMemory overwriteForeign);
void endCall(bool transferred, bool fellBack);
bool active();
bool lightingInput(uint32_t pc, unsigned base, unsigned count, unsigned context, uint8_t *data);
void lightingDone(uint8_t *data);
bool projectionInput(uint32_t pc, unsigned base, unsigned count, unsigned context, const float *firstMatrix,
                     uint8_t *data);
bool projecting();
void skippedProjection(unsigned count);
void projectionDone(uint8_t *data);
void ordinary();
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes);
void readMemory(const void *owner, uint8_t *data, unsigned first = 0, unsigned count = 1024);
void readW(const void *owner, uint8_t *data, unsigned first = 0, unsigned count = 1024);
void overwriteXYZ(const void *owner, uint8_t *data, unsigned first, unsigned count);
bool hasDeferred(const void *owner, const uint8_t *data);
void resetMemory(const void *owner);
Stats stats();
void finish();

template <class VU, class Body>
bool lighting(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, unsigned relative, Body body)
{
    if (!active())
        return body(v, data, size, end, ended, relative);
    const unsigned n = uint16_t(v.m_state.vi[10]), base = uint16_t(v.m_state.vi[1]);
    if (relative || size != 16384 || v.m_xgkick.active || v.m_state.branchPending || v.m_state.ebit ||
        v.m_state.haltAfterDelaySlot || v.m_stopRequested || v.m_state.dBitEnabled || v.m_state.tBitEnabled ||
        // Omitted FMAC and divide sticky flags must already be set. The
        // authenticated program divides positive literals and takes RSQRT of
        // a sum of squares, so these finite inputs cannot introduce invalid.
        (v.m_state.status & 0xbc0) != 0xbc0 || end < v.m_cycle || end - v.m_cycle < 4096 ||
        !lightingInput(v.m_state.pc, base, n, uint16_t(v.m_state.vi[11]), data))
    {
        ordinary();
        return body(v, data, size, end, ended, relative);
    }
    const unsigned target = (uint16_t(v.m_state.vi[15]) * 8u) & 16383u;
    v.advanceTo(v.m_cycle + (n - 4) * 41u);
    v.m_state.vi[1] = int16_t(base + (n - 4) * 3u);
    v.m_state.vi[10] = 4;
    if (!body(v, data, size, end, ended, relative) || ended || v.m_state.pc != target || v.m_stopRequested)
        throw std::runtime_error("Qualified reflection lighting tail changed control flow");
    v.m_state.vi[10] = int16_t(n);
    lightingDone(data);
    return true;
}
template <class VU, class Body>
bool projection(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, unsigned relative, Body body)
{
    if (!active())
        return body(v, data, size, end, ended, relative);
    if (relative || size != 16384 || v.m_xgkick.active || v.m_state.branchPending || v.m_state.ebit ||
        v.m_state.haltAfterDelaySlot || v.m_stopRequested || end < v.m_cycle || end - v.m_cycle < 2048 ||
        !projectionInput(v.m_state.pc, uint16_t(v.m_state.vi[1]), uint16_t(v.m_state.vi[10]),
                         uint16_t(v.m_state.vi[11]), v.m_state.vf[1], data))
    {
        ordinary();
        return body(v, data, size, end, ended, relative);
    }
    const unsigned target = (uint16_t(v.m_state.vi[15]) * 8u) & 16383u;
    if (!body(v, data, size, end, ended, relative) || ended || v.m_state.pc != target || v.m_stopRequested)
        throw std::runtime_error("Qualified reflection projection tail changed control flow");
    return true;
}
template <class VU, class Body>
bool projectionFirst(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, Body body)
{
    if (projecting())
    {
        unsigned skipped = 0;
        if (v.m_state.vi[13] > 0)
        {
            if (v.m_state.vi[10] != 16 || v.m_state.vi[13] < 3 || v.m_state.vi[13] > 6)
                throw std::runtime_error("Unexpected reflection projection block");
            v.advanceTo(v.m_cycle + 425u);
            for (unsigned i = 3; i <= 6; ++i)
                v.m_state.vi[i] = int16_t(v.m_state.vi[i] + 48);
            v.m_state.vi[10] = v.m_state.vi[13];
            v.m_state.vi[13] = 0;
            v.m_state.vi[8] = v.m_state.vi[9];
            skipped = 16;
        }
        const unsigned tailSkip = v.m_state.vi[10] > 4 ? v.m_state.vi[10] - 4 : 0;
        if (tailSkip)
        {
            v.advanceTo(v.m_cycle + tailSkip * 26u);
            for (unsigned i = 3; i <= 6; ++i)
                v.m_state.vi[i] = int16_t(v.m_state.vi[i] + tailSkip * 3u);
            v.m_state.vi[10] -= tailSkip;
            v.m_state.vi[8] = int16_t(uint16_t(v.m_state.vi[8]) << tailSkip);
            skipped += tailSkip;
        }
        skippedProjection(skipped);
    }
    return body(v, data, size, end, ended);
}
template <class VU, class Body>
bool projectionEnd(VU &v, uint8_t *data, uint32_t size, uint64_t end, bool &ended, Body body)
{
    if (projecting())
        projectionDone(data);
    return body(v, data, size, end, ended);
}
} // namespace tf_gpu_reflection
