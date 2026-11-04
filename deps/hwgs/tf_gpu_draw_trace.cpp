// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_gpu_draw_trace.h"
#include "kzgs_internal.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace
{
struct Trace
{
    FILE *file = nullptr;
    uint32_t field = 0, first = 3000, last = 3010;
    uint64_t draws = 0;
    bool capped = false;
} trace;

bool observe(void *context, GSHWDrawConfig &c)
{
    auto &t = *static_cast<Trace *>(context);
    if (t.field < t.first || t.field > t.last || !t.file)
        return false;
    if (t.draws == 100000)
    {
        if (!t.capped)
            std::fprintf(stderr, "[hwgs:draw-trace] row cap reached; capture is incomplete\n");
        t.capped = true;
        return false;
    }
    const int written = std::fprintf(
        t.file, "%u,%llu,%u,%u,%u,%u,%016llx,%016llx,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        t.field, static_cast<unsigned long long>(++t.draws), c.nverts, c.nindices, unsigned(c.topology),
        unsigned(c.vs.key), static_cast<unsigned long long>(c.ps.key_lo), static_cast<unsigned long long>(c.ps.key_hi),
        c.rt ? c.rt->GetWidth() : 0, c.rt ? c.rt->GetHeight() : 0, c.ds != nullptr, c.tex ? c.tex->GetWidth() : 0,
        c.tex ? c.tex->GetHeight() : 0, c.pal != nullptr, c.tex && (c.tex == c.rt || c.tex == c.ds),
        c.require_one_barrier, c.require_full_barrier, c.ps.IsSWBlending(), c.blend.enable, c.alpha_second_pass.enable,
        c.blend_multi_pass.enable, unsigned(c.ps.automatic_lod), unsigned(c.ps.manual_lod), unsigned(c.ps.fog),
        unsigned(c.ps.date), unsigned(c.depth.key), unsigned(c.colormask.key), c.line_expand);
    if (written < 0)
        throw std::runtime_error("Cannot write private GPU draw trace");
    return false; // Read-only: retain the enhanced renderer's ordinary submission.
}
} // namespace

void tfGpuDrawTraceOpen()
{
    const char *path = std::getenv("PS2X_TF_GPU_DRAW_TRACE");
    if (!path || !*path)
        return;
    kzgs::RunOnGSThread([&] {
        if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::D3D12)
            throw std::runtime_error("GPU draw tracing requires D3D12");
        if (std::filesystem::exists(path))
            throw std::runtime_error("Use a fresh private GPU draw trace path");
        trace = {};
        if (const char *range = std::getenv("PS2X_TF_GPU_DRAW_RANGE"))
        {
            char tail;
            if (std::sscanf(range, "%u:%u%c", &trace.first, &trace.last, &tail) != 2 || trace.last < trace.first ||
                trace.last - trace.first > 120)
                throw std::runtime_error("GPU draw trace range must span at most 120 fields");
        }
        if (fopen_s(&trace.file, path, "wx") || !trace.file)
            throw std::runtime_error("Cannot create private GPU draw trace");
        std::fputs("field,draw,vertices,indices,topology,vs,ps_lo,ps_hi,rt_width,rt_height,depth,tex_width,tex_height,"
                   "palette,feedback,one_barrier,full_barrier,software_blend,hardware_blend,alpha_second_pass,blend_"
                   "multi_pass,automatic_lod,manual_lod,fog,date,depth_key,color_mask,line_expand\n",
                   trace.file);
        GSDevice12::GetInstance()->SetNativeDrawHandler(&trace, observe);
    });
}
void tfGpuDrawTraceField()
{
    if (!trace.file)
        return;
    kzgs::RunOnGSThread([] {
        ++trace.field;
        if (trace.field == trace.last + 1 && std::fflush(trace.file))
            throw std::runtime_error("Cannot flush private GPU draw trace");
    });
}
void tfGpuDrawTraceClose()
{
    if (!trace.file)
        return;
    kzgs::RunOnGSThread([] {
        if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12)
            GSDevice12::GetInstance()->SetNativeDrawHandler(nullptr, nullptr);
        std::fprintf(stderr, "[hwgs:draw-trace] fields=%u range=%u:%u rows=%llu capped=%u\n", trace.field, trace.first,
                     trace.last, static_cast<unsigned long long>(trace.draws), trace.capped);
        std::fclose(trace.file);
        trace.file = nullptr;
    });
}
