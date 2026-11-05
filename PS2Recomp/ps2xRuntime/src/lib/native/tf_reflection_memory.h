// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_reflection_math.h"
#include <bitset>

namespace tf_native
{
// Access only under the existing VU memory owner/fence. GS queues have separate
// immutable companions. W lanes can be restored without geometry arithmetic.
class ReflectionDeferredMemory
{
    struct Cell
    {
        uint8_t slot = 255, vertex = 0, section = 0, kind = 0, mask = 0;
    };
    struct Batch
    {
        gsnative::DualPacketPtr source;
        unsigned remaining = 0;
        uint32_t alpha = 0;
        std::bitset<gsnative::ReflectionMaxVertices> litReady;
        std::bitset<gsnative::ReflectionMaxVertices * 4> projectedReady;
        std::array<gsnative::ReflectionLitVertex, gsnative::ReflectionMaxVertices> lit;
        std::array<gsnative::ReflectionRawVertex, gsnative::ReflectionMaxVertices * 4> projected;
    };
    std::array<Cell, 1024> cells{};
    std::array<Batch, 32> batches{};
    void discard(unsigned qword);

  public:
    uint64_t recorded = 0, overwrittenQwords = 0, materializedQwords = 0, preservedW = 0;
    uint64_t litVertices = 0, projectedVertices = 0;
    unsigned dirtyQwords = 0;
    void read(uint8_t *data, unsigned first = 0, unsigned count = 1024);
    void readW(uint8_t *data, unsigned first = 0, unsigned count = 1024);
    void overwrite(unsigned first, unsigned count);
    // Hardware writes every XYZ lane, possibly preserving W. Restore only W
    // before forgetting ownership, then let the caller write the new XYZ.
    void overwriteXYZ(uint8_t *data, unsigned first, unsigned count);
    // Call readW before execution for any output range whose W may be retained.
    // Record after the qualified CPU tail, before its XGKICK observes output.
    void record(uint8_t *data, unsigned base, unsigned resolvedTail, gsnative::DualPacketPtr source);
};
} // namespace tf_native
