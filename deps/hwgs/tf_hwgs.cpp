// SPDX-License-Identifier: GPL-3.0-or-later
// Transformers adapter to drakolordx7's kzgs and PCSX2. See NOTICE/THIRD_PARTY.md.
#include "tf_hwgs.h"
#ifdef TF_HWGS_GPU_GEOMETRY
#include "tf_gpu_draw_trace.h"
#include "tf_gpu_live_draw.h"
#endif
#include "kzgs.h"
#include "kzgs_internal.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <set>

namespace {
alignas(64) std::array<uint8_t, 8192> registers{};
KzgsConfig config;
std::vector<uint8_t> frame;
int frameWidth = 0, frameHeight = 0;
constexpr uint32_t vramBytes = 4u * 1024u * 1024u;
// Optional host bridge attribution. Called by the runtime GS worker only.
enum class Cost { Gif, Sync, Vsync, Readback, Rect, Pixel, RectWork, Count };
struct BridgeProfile {
    using Clock = std::chrono::steady_clock;
    bool enabled = [] { const char* p=std::getenv("PS2X_HWGS_PROFILE"); return p && *p=='1'; }();
    double ms[size_t(Cost::Count)]{};
    uint64_t calls[size_t(Cost::Count)]{}, fields=0;
    Clock::time_point last=Clock::now();
    void report() {
        if (!enabled || ++fields%300) return;
        const auto now=Clock::now();
        std::fprintf(stderr,"[hwgs:profile] fields=300 wall_ms=%.1f",std::chrono::duration<double,std::milli>(now-last).count());
        const char* names[]={"gif","sync","vsync","readback","rect","pixel","rect_work"};
        for(size_t i=0;i<size_t(Cost::Count);++i) {
            std::fprintf(stderr," %s=%llu/%.1fms",names[i],static_cast<unsigned long long>(calls[i]),ms[i]);
            calls[i]=0;ms[i]=0;
        }
        std::fprintf(stderr,"\n");last=now;
    }
} profile;
struct Timed {
    Cost cost; BridgeProfile::Clock::time_point start;
    explicit Timed(Cost c):cost(c) { if(profile.enabled)start=BridgeProfile::Clock::now(); }
    ~Timed(){if(profile.enabled){const auto i=size_t(cost);++profile.calls[i];profile.ms[i]+=std::chrono::duration<double,std::milli>(BridgeProfile::Clock::now()-start).count();}}
};

GIFRegBITBLTBUF blit(uint32_t psm, uint32_t base, uint32_t bw) {
    GIFRegBITBLTBUF b{};
    b.SPSM = psm & 63; b.SBP = base & 16383; b.SBW = bw & 63;
    b.DPSM = b.SPSM; b.DBP = b.SBP; b.DBW = b.SBW;
    return b;
}
void prepareRead(const GIFRegBITBLTBUF& b, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    g_gs_renderer->Flush(GSState::GSFlushReason::DOWNLOADFIFO);
    g_gs_renderer->InvalidateLocalMem(b, GSVector4i(x, y, x + w, y + h));
}
}

int tf_gs_open(int scale, int fxaa, const char* resources, char* error, uint32_t capacity) {
    if (kzgsIsOpen()) return 0;
    kzgsSetLogCallback([](int level, const char* message) {
        std::fprintf(stderr, "[hwgs:%d] %s\n", level, message);
    });
    config = KzgsConfig{};
    config.renderer = KzgsRenderer::Vulkan;
    // Private renderer comparison. Normal launches retain Vulkan until image and
    // performance qualification establishes another API as suitable for this game.
    if(const char* api=std::getenv("PS2X_HWGS_API");api && *api) {
        if(std::strcmp(api,"d3d12")==0)config.renderer=KzgsRenderer::D3D12;
        else if(std::strcmp(api,"vulkan")!=0) {
            if(error && capacity)std::snprintf(error,capacity,"PS2X_HWGS_API must be vulkan or d3d12");
            return 0;
        }
    }
    if(tf_gs_direct_enabled()) {
        const char* api=std::getenv("PS2X_HWGS_API");
        if(api && *api && std::strcmp(api,"d3d12")!=0) {
            if(error && capacity)std::snprintf(error,capacity,"direct GPU presentation requires d3d12");
            return 0;
        }
        config.renderer=KzgsRenderer::D3D12;
    }
    config.upscale = std::clamp(scale, 1, 8);
    config.fxaa = fxaa != 0;
    config.vsync = false; // The existing Transformers presenter owns pacing and the window.
    // PCSX2's pinned GameIndex.yaml, SLUS-20668: adaptive BFF avoids combing;
    // texture-in-target, native scaling and half-pixel fixes preserve shadows,
    // sky, light rays and depth edges when rendering above native resolution.
    config.interlaceMode = 9;
    config.halfPixelOffset = 5;
    config.nativeScaling = 2;
    config.textureInsideRt = 1;
    config.selfContainedGifPackets = true;
    config.resourcesDir = resources ? resources : "";
    if (resources && *resources) config.cacheDir=(std::filesystem::path(resources).parent_path()/"hwgs-cache").string();
    registers.fill(0);
    frame.clear(); frameWidth = frameHeight = 0;
    std::string message;
    const bool ok = kzgsOpen(nullptr, config, registers.data(), &message);
#ifdef TF_HWGS_GPU_GEOMETRY
    if (ok) tfGpuDrawTraceOpen();
#endif
    if (!ok && error && capacity) std::snprintf(error, capacity, "%s", message.c_str());
    return ok ? 1 : 0;
}
void tf_gs_close() {
#ifdef TF_HWGS_GPU_GEOMETRY
    tfGpuLiveDrawReset();
    tfGpuDrawTraceClose();
#endif
    if(kzgsIsOpen() && tf_gs_direct_enabled())tf_gs_direct_close();
    kzgsClose(); frame.clear(); frameWidth = frameHeight = 0;
}
void tf_gs_reset() {
#ifdef TF_HWGS_GPU_GEOMETRY
    tfGpuLiveDrawReset();
#endif
    kzgsReset(); frame.clear(); frameWidth = frameHeight = 0;
}
#ifndef TF_HWGS_GPU_GEOMETRY
int tf_gs_native_dual_gif(uint32_t, const uint8_t*, uint32_t, const gsnative::DualPacket*) { return 0; }
#endif
void tf_gs_sync() { Timed time(Cost::Sync); kzgsSync(); }
void tf_gs_configure(int scale, int fxaa) {
    config.upscale = std::clamp(scale, 1, 8); config.fxaa = fxaa != 0;
    kzgsUpdateConfig(config); kzgsSync();
}
void tf_gs_gif(uint32_t path, const uint8_t* data, uint32_t bytes) {
    Timed time(Cost::Gif);
    if (data && bytes && !(bytes & 15) && path >= 1 && path <= 3)
        kzgsGifTransfer(static_cast<int>(path), data, bytes / 16);
}
void tf_gs_vsync(const uint8_t* priv) {
    if (!priv || !kzgsIsOpen()) return;
#ifdef TF_HWGS_GPU_GEOMETRY
    tfGpuDrawTraceField();
#endif
    std::memcpy(registers.data(), priv, registers.size());
    // Reserved runtime metadata is not a GS register.
    std::memset(registers.data() + 0xf00, 0, 8);
    std::memset(registers.data() + 0xff8, 0, 8);
    uint64_t csr = 0; std::memcpy(&csr, registers.data() + 0x1000, 8);
    { Timed time(Cost::Vsync); kzgsVsync((csr & 0x2000) ? 0 : 1, true); }
    if(!tf_gs_direct_enabled()) {
        Timed time(Cost::Readback);
        if (!kzgsReadback(frame, frameWidth, frameHeight)) {
            frame.clear(); frameWidth = frameHeight = 0;
        }
    }
    profile.report();
}
uint32_t tf_gs_frame(uint8_t* data, uint32_t capacity, uint32_t* width, uint32_t* height) {
    if (width) *width = std::max(0, frameWidth);
    if (height) *height = std::max(0, frameHeight);
    if (frame.size() > UINT32_MAX) return 0;
    const auto bytes = static_cast<uint32_t>(frame.size());
    if (data && capacity >= bytes && bytes) std::memcpy(data, frame.data(), bytes);
    return bytes;
}
uint32_t tf_gs_read_rect(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, uint8_t* data, uint32_t capacity) {
    Timed time(Cost::Rect);
    if(profile.enabled) {
        static std::set<std::array<uint32_t,7>> seen;
        const std::array<uint32_t,7> key{psm,base,bw,x,y,w,h};
        if(seen.size()<32 && seen.insert(key).second)
            std::fprintf(stderr,"[hwgs:rect] psm=%u base=%u bw=%u x=%u y=%u w=%u h=%u\n",psm,base,bw,x,y,w,h);
    }
    if (!kzgsIsOpen() || !data || psm >= 64 || !w || !h || w > 4096 || h > 4096) return 0;
    uint32_t bytes = 0;
    kzgs::RunOnGSThread([&] {
        // The producer waits for this callback, so profiling is still serialized.
        // Separates actual GS/GPU read work from waiting behind queued GIF packets.
        Timed work(Cost::RectWork);
        const auto bits = GSLocalMemory::m_psm[psm].trbpp;
        if (!bits) return;
        const uint64_t required = (uint64_t(w) * h * bits + 7) / 8;
        if (required > capacity || required > vramBytes) return;
        auto b = blit(psm, base, bw);
        GIFRegTRXPOS pos{}; pos.SSAX = x; pos.SSAY = y;
        GIFRegTRXREG reg{}; reg.RRW = w; reg.RRH = h;
        prepareRead(b, x, y, w, h);
        int tx = x, ty = y;
        std::memset(data, 0, static_cast<size_t>(required));
        if (bits == 4) {
            // PCSX2's optimized transfer path assumes paired texels. The host API also
            // permits odd origins/widths, so pack nibbles explicitly across row boundaries.
            const auto read = GSLocalMemory::m_psm[psm].rp;
            if (!read) return;
            for (uint64_t i = 0; i < uint64_t(w)*h; ++i) {
                const auto pixel = (g_gs_renderer->m_mem.*read)(x+uint32_t(i%w),y+uint32_t(i/w),b.SBP,b.SBW);
                data[i/2] |= uint8_t((pixel & 15) << ((i&1)*4));
            }
        } else {
            g_gs_renderer->m_mem.ReadImageX(tx, ty, data, static_cast<int>(required), b, pos, reg);
        }
        bytes = static_cast<uint32_t>(required);
    });
    return bytes;
}
uint32_t tf_gs_read_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) {
    Timed time(Cost::Pixel);
    uint32_t result = 0;
    if (!kzgsIsOpen() || psm >= 64) return result;
    kzgs::RunOnGSThread([&] {
        auto b = blit(psm, base, bw); prepareRead(b, x, y, 1, 1);
        const auto fn = GSLocalMemory::m_psm[psm].rp;
        if (fn) result = (g_gs_renderer->m_mem.*fn)(x, y, base, bw);
    });
    return result;
}
void tf_gs_write_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) {
    if (!kzgsIsOpen() || psm >= 64) return;
    kzgs::RunOnGSThread([&] {
        auto b = blit(psm, base, bw); prepareRead(b, x, y, 1, 1);
        const auto fn = GSLocalMemory::m_psm[psm].wp;
        if (fn) {
            (g_gs_renderer->m_mem.*fn)(x, y, value, base, bw);
            g_gs_renderer->InvalidateVideoMem(b, GSVector4i(x, y, x + 1, y + 1));
        }
    });
}
void tf_gs_snapshot(uint8_t* data, uint32_t capacity) {
    if (!kzgsIsOpen() || !data || capacity < vramBytes) return;
    kzgs::RunOnGSThread([&] {
        g_gs_renderer->Flush(GSState::GSFlushReason::SAVESTATE);
        g_gs_renderer->ReadbackTextureCache();
        std::memcpy(data, g_gs_renderer->m_mem.m_vm8, vramBytes);
    });
}
void tf_gs_upload_vram(const uint8_t* data, uint32_t bytes) {
    if (!kzgsIsOpen() || !data || bytes != vramBytes) return;
    kzgs::RunOnGSThread([&] {
        g_gs_renderer->Flush(GSState::GSFlushReason::UPLOADDIRTYFRAME);
        std::memcpy(g_gs_renderer->m_mem.m_vm8, data, bytes);
        g_gs_renderer->InvalidateVideoMem(blit(0, 0, 32), GSVector4i(0, 0, 2048, 512));
    });
}
