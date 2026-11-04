// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_dual_frontend.h"
#include <cmath>
#include <limits>

namespace tf_gpu
{
bool prepareDualTrace(const DualDrawBounds &b, const GSDrawingContext &ctx, const GIFRegPRIM &prim,
                      GSVertexTrace::Vertex &minimum, GSVertexTrace::Vertex &maximum)
{
    if (!prim.TME || !prim.IIP || prim.FST || prim.AA1 || b.wrappedAxes)
        return false;
    const auto outward = [](double value, bool upper) {
        return std::nextafter(static_cast<float>(value),
                              upper ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity());
    };
    minimum = {};
    maximum = {};
    const uint32_t offset[] = {ctx.XYOFFSET.OFX, ctx.XYOFFSET.OFY};
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (b.xyzMin[axis] > b.xyzMax[axis] || b.xyzMax[axis] > (axis == 2 ? 0xffffffu : 65535u))
            return false;
        minimum.p.F32[axis] = axis == 2 ? float(b.xyzMin[axis]) : (int(b.xyzMin[axis]) - int(offset[axis])) / 16.0f;
        maximum.p.F32[axis] = axis == 2 ? float(b.xyzMax[axis]) : (int(b.xyzMax[axis]) - int(offset[axis])) / 16.0f;
    }
    minimum.p.w = 0;
    maximum.p.w = 255;
    for (unsigned channel = 0; channel < 4; ++channel)
    {
        if (b.colorMin[channel] > b.colorMax[channel] || b.colorMax[channel] > 255)
            return false;
        if (!(ctx.TEX0.TFX == TFX_DECAL && ctx.TEX0.TCC))
        {
            minimum.c.I32[channel] = b.colorMin[channel];
            maximum.c.I32[channel] = b.colorMax[channel];
        }
        const double scale = channel < 2 ? double(1u << (channel ? ctx.TEX0.TH : ctx.TEX0.TW)) : 1.0;
        const auto &range = channel < 2 ? b.texture[channel] : b.stq[2];
        minimum.t.F32[channel] = outward(range.low * scale, false);
        maximum.t.F32[channel] = outward(range.high * scale, true);
        if (!std::isfinite(minimum.t.F32[channel]) || !std::isfinite(maximum.t.F32[channel]) ||
            minimum.t.F32[channel] > maximum.t.F32[channel])
            return false;
    }
    return minimum.t.z > 0 && maximum.t.z < 1e30f;
}
} // namespace tf_gpu
