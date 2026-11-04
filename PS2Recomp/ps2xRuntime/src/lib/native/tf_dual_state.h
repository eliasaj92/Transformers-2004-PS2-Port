// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

struct VU1State;

namespace tf_native {
// Candidate preservation effects for an already identified, unclipped
// DualTransform continuation. This is not a complete VU-state replacement.
// GPU-only XYZ/STQ words remain unresolved until overwritten or materialized.
struct DualPreservation {
    std::array<uint32_t, 4> vf17{}, vf18{};
    std::array<uint16_t, 112> geometryQwords{};
    uint32_t geometryQwordCount = 0;
    uint64_t guestCycles = 0;
};

// Applies packet/color/padding bookkeeping to the caller's private VU data copy.
// Callers must qualify program identity and later readers before runtime use.
bool preserveUnclippedDualState(uint8_t* data, const VU1State& state,
    uint32_t itop, uint32_t context, uint32_t entry, DualPreservation& effects);
}
