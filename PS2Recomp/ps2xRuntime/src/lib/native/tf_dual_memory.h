// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_dual_math.h"
#include <array>
#include <bitset>

namespace tf_native
{
// All calls run under the existing VU-memory ownership/fence protocol. GS
// queues own independent immutable source packets and never touch this map.
class DualDeferredMemory
{
    struct Cell
    {
        uint8_t slot = 255, vertex = 0, field = 0, xyz = 0;
    };
    struct Batch
    {
        gsnative::DualPacketPtr source;
        unsigned remaining = 0;
        std::bitset<56> computed;
        std::array<gsnative::DualRawVertex, 56> cache;
    };
    std::array<Cell, 1024> cells{};
    std::array<Batch, 32> batches{};
    void discard(unsigned qword);

  public:
    uint64_t recorded = 0, overwrittenQwords = 0, materializedVertices = 0, materializedQwords = 0;
    unsigned dirtyQwords = 0;
    void read(uint8_t *data, unsigned firstQword = 0, unsigned count = 1024);
    void overwrite(unsigned firstQword, unsigned count);
    void record(uint8_t *data, uint32_t itop, uint32_t resolvedTail, gsnative::DualPacketPtr source);
};
} // namespace tf_native
