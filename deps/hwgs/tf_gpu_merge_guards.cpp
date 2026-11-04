// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_merge_guards.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include <algorithm>
#include <cstdio>

namespace tf_gpu
{
namespace
{
struct Blocks
{
    uint32_t first, end;
};
bool overlaps(Blocks a, Blocks b)
{
    // Ranges are unwrapped; a texture can straddle the end of GS VRAM.
    constexpr uint32_t size = GS_MAX_BLOCKS;
    return (a.first < b.end && b.first < a.end) || (a.first + size < b.end && b.first < a.end + size) ||
           (a.first < b.end + size && b.first + size < a.end);
}
bool blocks(uint32_t base, uint32_t width, uint32_t psm, uint32_t x, uint32_t y, Blocks &result)
{
    switch (psm)
    {
    case PSMCT32:
    case PSMCT24:
    case PSMCT16:
    case PSMCT16S:
    case PSMT8:
    case PSMT4:
    case PSMT8H:
    case PSMT4HL:
    case PSMT4HH:
    case PSMZ32:
    case PSMZ24:
    case PSMZ16:
    case PSMZ16S:
        break;
    default:
        return false;
    }
    if (!width || !x || !y || x > width * 64 || x > 2048 || y > 2048)
        return false;
    const auto page = GSLocalMemory::m_psm[psm].pgs;
    const uint32_t rows = (y + page.y - 1) / page.y;
    const uint32_t stride = std::max(1u, (width * 64) / uint32_t(page.x));
    if (uint64_t(rows) * stride >= GS_MAX_PAGES)
        return false;
    const auto rect = GSVector4i(0, 0, int(x), int(y)).ralign<Align_Outside>(page);
    result.first = base & ~31u;
    // The GS endpoint helper's page-aligned rectangle rule assumes an aligned
    // base. An unaligned texture can spill into one additional page; compute
    // from the aligned base and include that page explicitly.
    result.end = ((GSLocalMemory::GetUnwrappedEndBlockAddress(result.first, width, psm, rect) + 32) & ~31u) +
                 (base % 32 ? 32 : 0);
    return result.end > result.first && result.end - result.first < GS_MAX_BLOCKS;
}
} // namespace
bool noFieldTextureFeedback(const GSDrawingContext &context, const GIFRegSCISSOR &a, const GIFRegSCISSOR &b)
{
    const unsigned width = std::max(a.SCAX1, b.SCAX1) + 1;
    const unsigned height = std::max(a.SCAY1, b.SCAY1) + 1;
    Blocks frame{}, depth{};
    if (!blocks(context.FRAME.Block(), context.FRAME.FBW, context.FRAME.PSM, width, height, frame) ||
        !blocks(context.ZBUF.Block(), context.FRAME.FBW, context.ZBUF.PSM, width, height, depth) ||
        frame.end > GS_MAX_BLOCKS || depth.end > GS_MAX_BLOCKS || overlaps(frame, depth))
        return false;
    const auto safe = [&](Blocks range) { return !overlaps(range, frame) && !overlaps(range, depth); };
    const auto &tex = context.TEX0;
    if (tex.TW > 10 || tex.TH > 10 || context.TEX1.MXL > 6)
        return false;
    const uint32_t bases[] = {uint32_t(tex.TBP0),
                              uint32_t(context.MIPTBP1.TBP1),
                              uint32_t(context.MIPTBP1.TBP2),
                              uint32_t(context.MIPTBP1.TBP3),
                              uint32_t(context.MIPTBP2.TBP4),
                              uint32_t(context.MIPTBP2.TBP5),
                              uint32_t(context.MIPTBP2.TBP6)};
    const uint32_t widths[] = {uint32_t(tex.TBW),
                               uint32_t(context.MIPTBP1.TBW1),
                               uint32_t(context.MIPTBP1.TBW2),
                               uint32_t(context.MIPTBP1.TBW3),
                               uint32_t(context.MIPTBP2.TBW4),
                               uint32_t(context.MIPTBP2.TBW5),
                               uint32_t(context.MIPTBP2.TBW6)};
    for (unsigned lod = 0; lod <= context.TEX1.MXL; ++lod)
    {
        unsigned x = std::max(1u, (1u << tex.TW) >> lod), y = std::max(1u, (1u << tex.TH) >> lod);
        // Region clamp/repeat can address texels beyond TEX0's nominal size.
        if (context.CLAMP.WMS >= 2)
            x = std::max(x, unsigned(std::max(context.CLAMP.MINU, context.CLAMP.MAXU)) + 1);
        if (context.CLAMP.WMT >= 2)
            y = std::max(y, unsigned(std::max(uint32_t(context.CLAMP.MINV), uint32_t(context.CLAMP.MAXV))) + 1);
        Blocks texture{};
        if (!blocks(bases[lod], widths[lod], tex.PSM, x, y, texture) || !safe(texture))
            return false;
    }
    if (GSLocalMemory::m_psm[tex.PSM].pal)
    {
        // CSM1 uses at most 512 32-bit entries. Cover two full pages including
        // an unaligned CBP. CSM2 addressing requires a different proof.
        if (tex.CSM || !safe({uint32_t(tex.CBP) & ~31u, (uint32_t(tex.CBP) & ~31u) + 64}))
            return false;
    }
    return true;
}
void mergeDrawBounds(DualDrawBounds &into, const DualDrawBounds &other)
{
    for (unsigned i = 0; i < 3; ++i)
    {
        into.xyzMin[i] = std::min(into.xyzMin[i], other.xyzMin[i]);
        into.xyzMax[i] = std::max(into.xyzMax[i], other.xyzMax[i]);
        into.stq[i].low = std::min(into.stq[i].low, other.stq[i].low);
        into.stq[i].high = std::max(into.stq[i].high, other.stq[i].high);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        into.texture[i].low = std::min(into.texture[i].low, other.texture[i].low);
        into.texture[i].high = std::max(into.texture[i].high, other.texture[i].high);
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        into.colorMin[i] = std::min(into.colorMin[i], other.colorMin[i]);
        into.colorMax[i] = std::max(into.colorMax[i], other.colorMax[i]);
    }
    into.wrappedAxes |= other.wrappedAxes;
}
bool probeMergeRanges(uint64_t &checks)
{
    for (unsigned psm :
         {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S})
        for (unsigned base : {0u, 1u, 31u, 32u, 255u, 1023u, 3584u, 10752u, 15360u, 16351u, 16383u})
            for (unsigned stride : {1u, 2u, 3u, 8u, 16u})
                for (unsigned width : {1u, 31u, 63u, 64u, 65u, 127u, 128u, 511u, 512u, 1024u})
                    for (unsigned height : {1u, 31u, 32u, 63u, 64u, 447u, 1024u})
                    {
                        Blocks range{};
                        if (!blocks(base, stride, psm, width, height, range))
                            continue;
                        bool contains = true;
                        auto offset = g_gs_renderer->m_mem.GetOffset(base, stride, psm);
                        offset.loopPages(GSVector4i(0, 0, int(width), int(height)), [&](uint32_t page) {
                            const uint32_t block = page * 32;
                            contains &=
                                (block >= range.first && block + 32 <= range.end) ||
                                (block + GS_MAX_BLOCKS >= range.first && block + GS_MAX_BLOCKS + 32 <= range.end);
                        });
                        if (!contains)
                        {
                            std::fprintf(stderr,
                                         "[gpu:grass-range] psm=%u base=%u stride=%u width=%u height=%u range=%u:%u\n",
                                         psm, base, stride, width, height, range.first, range.end);
                            return false;
                        }
                        ++checks;
                    }
    return true;
}

} // namespace tf_gpu
