// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Handwritten geometry model for Transformers' grass triangle strips.
// Offline/live qualification only: not a VU or renderer replacement.
namespace tf_native {
using GrassVector = std::array<float, 4>;
using GrassWords = std::array<uint32_t, 4>;
struct GrassVertex {
    GrassVector position;
    GrassWords color;
};
struct GrassContext {
    GrassVector clipScale, clipOffset, billboardOffset;
    std::array<std::array<GrassVector, 4>, 2> transform;
    float depthBias, density, distanceScale;
    std::array<GrassVector, 32> scatter;
};
struct GrassStrip {
    std::span<const GrassVertex> vertices;
    std::array<uint16_t, 2> restartMasks;
};
struct GrassBatch {
    // Eight sprites per field, two XYZ/clip words per sprite, in GS order.
    std::array<std::array<GrassWords, 16>, 2> positions;
    std::array<GrassWords, 8> colors;
};
struct GrassStats {
    uint32_t triangles = 0, coarseCulled = 0, subdivisions = 0, maxDepth = 0;
};

// Requires compilation without floating-point contraction/reassociation.
// Uses toward-zero arithmetic and restores the caller's floating environment.
// Unsupported input or exceeded bounds returns false and clears output.
// Does not reproduce GS state, packet templates, or persistent VU state.
bool buildGrassGeometry(const GrassContext& context, const GrassStrip& strip,
                        std::vector<GrassBatch>& output, GrassStats& stats);
}
