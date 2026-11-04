// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_gpu_dual_bounds.h"
#include "GS/Renderers/Common/GSVertexTrace.h"

namespace tf_gpu
{
// Convert cached object metadata into GS trace units for textured IIP triangles.
// This does not prove nonconstant depth, GIF ordering, or VU-state ownership.
bool prepareDualTrace(const DualDrawBounds &bounds, const GSDrawingContext &context, const GIFRegPRIM &prim,
                      GSVertexTrace::Vertex &minimum, GSVertexTrace::Vertex &maximum);
bool prepareGrassTrace(const DualDrawBounds &bounds, const GSDrawingContext &context, const GIFRegPRIM &prim,
                       GSVertexTrace::Vertex &minimum, GSVertexTrace::Vertex &maximum);
} // namespace tf_gpu
