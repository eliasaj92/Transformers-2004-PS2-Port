// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "tf_gpu_dual_frontend.h"

namespace tf_gpu
{
// The caller separately proves disjoint field scissors and identical state.
// Reject target aliasing/wrap and texture, mipmap or CLUT target feedback.
bool noFieldTextureFeedback(const GSDrawingContext &context, const GIFRegSCISSOR &a, const GIFRegSCISSOR &b);
void mergeDrawBounds(DualDrawBounds &into, const DualDrawBounds &other);
bool probeMergeRanges(uint64_t &checks);
} // namespace tf_gpu
