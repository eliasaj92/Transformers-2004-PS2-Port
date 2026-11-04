// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_grass_math.h"
#include <bitset>

namespace tf_native
{
// VU-owner-only storage for skipped grass XYZF2 and RGBA qwords. Unlike dual
// geometry, all four lanes are owned: an XYZ-only write must first read these
// cells to preserve clipping/color W. Queued GS packets own separate companions.
class GrassDeferredMemory
{
    struct Cell
    {
        uint8_t slot = 255, sprite = 0, field = 0, kind = 0;
    };
    struct Batch
    {
        gsnative::DualPacketPtr source;
        unsigned remaining = 0;
        std::bitset<16> computed;
        std::array<gsnative::GrassRawSprite, 16> cache;
    };
    std::array<Cell, 1024> cells{};
    std::array<Batch, 8> batches{};
    void discard(unsigned qword);

  public:
    uint64_t recorded = 0, overwrittenQwords = 0, materializedSprites = 0, materializedQwords = 0;
    unsigned dirtyQwords = 0;
    void read(uint8_t *data, unsigned firstQword = 0, unsigned count = 1024);
    void overwrite(unsigned firstQword, unsigned count);
    // Called after the accelerated epilogue: the last sprite pair and framing
    // are complete, and no other guest code has observed this output. Each loop
    // iteration finishes the previous pair, so the prologue is deferred too.
    void record(uint8_t *data, unsigned packetQword, gsnative::DualPacketPtr source);
};
} // namespace tf_native
