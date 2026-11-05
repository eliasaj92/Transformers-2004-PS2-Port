// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
namespace gsnative { struct DualPacket; }

namespace tf_gpu
{
struct DualRange
{
    double low = 0, high = 0;
};

// Computed once from immutable, untransformed inputs. No camera or GPU output.
struct DualAttributes
{
    std::array<DualRange, 3> position{}, stq{};
    std::array<uint32_t, 4> colorMin{}, colorMax{};
    bool valid = false;
};

struct DualDrawBounds
{
    // Native GS packed coordinates, before XYOFFSET and texture dimensions.
    std::array<uint32_t, 3> xyzMin{}, xyzMax{};
    std::array<DualRange, 3> stq{};
    std::array<DualRange, 2> texture{}; // S/Q and T/Q, including rounding error.
    std::array<uint32_t, 4> colorMin{}, colorMax{};
    uint32_t wrappedAxes = 0; // Wrapped coordinate ranges conservatively cover the full axis.
};

bool summarizeDualInputs(const void *inputs, uint32_t vertices, DualAttributes &result);
void mergeDualAttributes(DualAttributes &result, const DualAttributes &other);
// Projects an object box's eight corners, with bounds on each VU RTZ/FTZ
// arithmetic step. Work is independent of the mesh's vertex count. Declines
// non-finite/overflowing inputs, boxes crossing W=0, and nonpositive output Q.
// This supplies bounds, not all GS draw state or permission to skip VU work.
bool projectDualBounds(const DualAttributes &attributes, const void *matrix, uint32_t bias, DualDrawBounds &result);
// Bounds from one 320-byte grass emitter input, without emitting its vertices.
// Uses monotone binary32 interval operations in the VU rounding environment.
bool projectGrassBounds(const void *inputs, std::array<DualDrawBounds, 2> &result);
// Source-space reflection boxes are split only when normalization/Q bounds
// are inconclusive. Never reads GPU output or emits projected CPU vertices.
bool projectReflectionBounds(const gsnative::DualPacket &packet, std::array<DualDrawBounds, 4> &result,
                             uint32_t *visitedNodes = nullptr, uint32_t *acceptedLeaves = nullptr);
} // namespace tf_gpu
