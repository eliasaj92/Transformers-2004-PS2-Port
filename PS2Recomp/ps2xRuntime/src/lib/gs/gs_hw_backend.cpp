// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_hw_backend.h"
#include "gs_hw_present.h"
#include "runtime/ps2_settings.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#ifdef PS2X_WITH_HWGS
#include "tf_hwgs.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
std::mutex sharpMutex;
std::vector<uint8_t> sharpPixels;
uint32_t sharpWidth = 0, sharpHeight = 0;
std::atomic<uint32_t> nativeWidth{0};
std::atomic<int> presentedScene{-1};
constexpr uint32_t vramBytes = 4u * 1024u * 1024u;
uint32_t transferBytes(uint32_t psm, uint32_t w, uint32_t h) {
    const uint32_t bits = psm == 0x14 || psm == 0x24 || psm == 0x2c ? 4 :
        psm == 0x13 || psm == 0x1b ? 8 :
        psm == 2 || psm == 0x0a || psm == 0x32 || psm == 0x3a ? 16 :
        psm == 1 || psm == 0x31 ? 24 : 32;
    const uint64_t bytes = (uint64_t(w) * h * bits + 7) / 8;
    if (bytes > vramBytes) throw std::runtime_error("HWGS: invalid transfer extent");
    return static_cast<uint32_t>(bytes);
}

#ifdef PS2X_WITH_HWGS
class HardwareGS final : public GSRasterBackend {
    bool initialized = false;
    int scale = 4, fxaa = 0;
    uint64_t ticks = 0, lastSceneTick = 0;
    uint32_t loggedWidth = 0, loggedHeight = 0;
    std::mutex frameMutex;
    PresentationFrame mailbox;
    bool mailboxScene = false;
    int mailboxScale = 1;
    GSTransferSnapshot transfer{};
    std::vector<uint8_t> fifo;
    size_t fifoPosition = 0;
public:
    ~HardwareGS() override { if (initialized) tf_gs_close(); }
    bool UsesRawGIF() const override { return true; }
    bool WantsDecodedRaster() const override { return false; }
    void Initialize(uint8_t* vram, uint32_t bytes) override {
        if (!vram || !bytes) return;
        if (bytes < vramBytes) throw std::runtime_error("HWGS requires 4 MiB VRAM");
        if (!initialized) {
            wchar_t executable[32768]{};
            if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error("HWGS: executable path unavailable");
            auto resources = (std::filesystem::path(executable).parent_path() / "resources").string();
            if (const char* p = std::getenv("PS2X_HWGS_RESOURCES")) resources = p;
            scale = std::clamp(ps2x::settings::getInt("renderer.hw_scale"), 1, 8);
            fxaa = ps2x::settings::getBool("renderer.hw_fxaa") ? 1 : 0;
            char error[2048]{};
            if (!tf_gs_open(scale, fxaa, resources.c_str(), error, sizeof(error)))
                throw std::runtime_error(std::string("HWGS: ") + error);
            initialized = true;
            ps2x::settings::declareWired("hardware-gs");
        }
        tf_gs_upload_vram(vram, vramBytes);
    }
    void Reset() override {
        if (initialized) tf_gs_reset();
        transfer = {}; fifo.clear(); fifoPosition = 0; ticks = lastSceneTick = 0;
        {std::lock_guard<std::mutex> lock(frameMutex); mailbox = {};}
        {std::lock_guard<std::mutex> lock(sharpMutex);sharpPixels.clear();sharpWidth=sharpHeight=0;}
        nativeWidth.store(0);presentedScene.store(-1);
    }
    void RawGIF(uint32_t path, const uint8_t* data, uint32_t bytes) override {
        if (!initialized || !data || !bytes) return;
        if (bytes & 15) throw std::runtime_error("HWGS: unaligned GIF packet");
        if (path < 1 || path > 3) path = 3;
        if (path == 1) lastSceneTick = ticks + 1;
        tf_gs_gif(path, data, bytes);
    }
    void RawAD(uint8_t reg, uint64_t value) override {
        if (!initialized || reg >= 128) return;
        // Runtime-only aliases for privileged registers are supplied in RawVsync.
        if ((reg >= 0x59 && reg <= 0x5c) || reg == 0x5f) return;
        alignas(16) const uint64_t packet[] = {uint64_t(1) | (uint64_t(1) << 15) | (uint64_t(1) << 60), 0x0e, value, reg};
        tf_gs_gif(3, reinterpret_cast<const uint8_t*>(packet), sizeof(packet));
    }
    void RawVsync(const std::array<uint8_t,8192>& regs) override {
        if (!initialized) return;
        ++ticks;
        const int nextScale = std::clamp(ps2x::settings::getInt("renderer.hw_scale"), 1, 8);
        const int nextFxaa = ps2x::settings::getBool("renderer.hw_fxaa") ? 1 : 0;
        if (scale != nextScale || fxaa != nextFxaa) {
            scale = nextScale; fxaa = nextFxaa; tf_gs_configure(scale, fxaa);
        }
        tf_gs_vsync(regs.data());
        uint64_t hold = 0; std::memcpy(&hold, regs.data() + 0xff8, 8);
        if (hold == 0x504c4f4144484c44ull) return;
        uint64_t display = 0; std::memcpy(&display, regs.data() + 0x70, 8);
        if(tf_gs_direct_enabled()) {
            const bool scene=lastSceneTick && ticks-std::min(ticks,lastSceneTick)<8;
            if(!tf_gs_direct_publish(scale,scene?1:0,uint32_t(display & 511),ticks))
                throw std::runtime_error("HWGS direct GPU presentation failed");
            return;
        }
        PresentationFrame f;
        const uint32_t size = tf_gs_frame(nullptr, 0, &f.width, &f.height);
        if (!size || !f.width || !f.height || size != uint64_t(f.width)*f.height*4) return;
        f.pixels.resize(size); tf_gs_frame(f.pixels.data(), size, nullptr, nullptr);
        // Keep the host's blending behavior consistent with the reference presenter.
        for (size_t i = 3; i < f.pixels.size(); i += 4) f.pixels[i] = 255;
        f.displayFbp = f.sourceFbp = uint32_t(display & 511);
        if (f.width != loggedWidth || f.height != loggedHeight) {
            loggedWidth = f.width; loggedHeight = f.height;
            std::fprintf(stderr,"[hwgs] rendered=%ux%u scale=%d fxaa=%d\n",f.width,f.height,scale,fxaa);
        }
        std::lock_guard<std::mutex> lock(frameMutex);
        mailbox = std::move(f); mailboxScale = scale;
        mailboxScene = lastSceneTick && ticks - std::min(ticks,lastSceneTick) < 8;
    }
    PresentationFrame Present(const GSPresentationRequest&) override {
        PresentationFrame high; int frameScale; bool scene;
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            if (!mailbox) return {};
            high = std::move(mailbox); mailbox = {}; frameScale = mailboxScale; scene = mailboxScene;
        }
        // Existing script/image consumers use a 640-pixel native stride. Publish the full
        // raster separately for UploadFrame, associated with this exact consumed frame.
        PresentationFrame low;
        low.width = std::min(640u,std::max(1u,high.width/uint32_t(frameScale)));
        low.height = std::min(512u,std::max(1u,high.height/uint32_t(frameScale)));
        low.displayFbp = high.displayFbp; low.sourceFbp = high.sourceFbp;
        low.pixels.assign(size_t(640)*low.height*4,0);
        for (uint32_t y=0;y<low.height;++y) for (uint32_t x=0;x<low.width;++x) {
            const uint32_t sx=uint32_t(uint64_t(x)*high.width/low.width), sy=uint32_t(uint64_t(y)*high.height/low.height);
            std::memcpy(low.pixels.data()+(size_t(y)*640+x)*4, high.pixels.data()+(size_t(sy)*high.width+sx)*4,4);
        }
        {
            std::lock_guard<std::mutex> lock(sharpMutex);
            sharpPixels = std::move(high.pixels); sharpWidth=high.width; sharpHeight=high.height;
        }
        nativeWidth.store(low.width); presentedScene.store(scene ? 1 : 0);
        return low;
    }
    void Submit(const GSPrimitiveBatch&) override {}
    void LoadClut(const GSTex0Reg&,const GSTexClutReg&) override {}
    void UploadImage(const uint8_t*,uint32_t) override {} // Already delivered in the raw GIF stream.
    void TextureFlush() override {} // TEXFLUSH is already in the raw stream.
    void Flush() override { if (initialized) tf_gs_sync(); }
    void Sync(GSSyncReason) override { if (initialized) tf_gs_sync(); }
    bool ClearFramebuffer(const GSContext&,uint32_t) override { return true; } // HLE emits a raw clear sprite.
    void BeginTransfer(const GSTransferCommand& c) override {
        transfer={}; fifo.clear(); fifoPosition=0;
        transfer.direction=c.direction; transfer.totalPixels=uint32_t(c.trxreg.rrw)*c.trxreg.rrh;
        transfer.x=c.trxpos.dsax; transfer.y=c.trxpos.dsay;
        if(c.direction!=1 || !initialized) return;
        transfer.x=c.trxpos.ssax; transfer.y=c.trxpos.ssay;
        fifo.resize(transferBytes(c.bitbltbuf.spsm,c.trxreg.rrw,c.trxreg.rrh));
        if(!fifo.empty() && tf_gs_read_rect(c.bitbltbuf.spsm,c.bitbltbuf.sbp,c.bitbltbuf.sbw,
            c.trxpos.ssax,c.trxpos.ssay,c.trxreg.rrw,c.trxreg.rrh,fifo.data(),uint32_t(fifo.size()))!=fifo.size())
            throw std::runtime_error("HWGS: local-to-host readback failed");
        transfer.localToHostPendingBytes=fifo.size();
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t* dst,uint32_t bytes) override {
        if(!dst) return 0;
        const auto n=uint32_t(std::min<size_t>(bytes,fifo.size()-fifoPosition));
        if(n) std::memcpy(dst,fifo.data()+fifoPosition,n);
        fifoPosition+=n; transfer.localToHostPendingBytes=fifo.size()-fifoPosition;
        if(!transfer.localToHostPendingBytes) transfer.copiedPixels=transfer.totalPixels;
        return n;
    }
    bool ReadRectAsync(uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y,uint32_t w,uint32_t h,
                       std::function<void(std::vector<uint8_t>)> complete) override {
        if(!initialized || !complete || !w || !h) return false;
        std::vector<uint8_t> bytes(transferBytes(psm,w,h));
        if(tf_gs_read_rect(psm,base,bw,x,y,w,h,bytes.data(),uint32_t(bytes.size()))!=bytes.size()) return false;
        complete(std::move(bytes)); return true;
    }
    uint32_t ReadVram(uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y) const override {
        return initialized ? tf_gs_read_pixel(psm,base,bw,x,y) : 0;
    }
    void WriteVram(uint32_t psm,uint32_t base,uint32_t bw,uint32_t x,uint32_t y,uint32_t value) override {
        if(initialized) tf_gs_write_pixel(psm,base,bw,x,y,value);
    }
    void SnapshotVram(std::vector<uint8_t>& out) const override {
        out.assign(vramBytes,0); if(initialized) tf_gs_snapshot(out.data(),vramBytes);
    }
    GSTransferSnapshot GetTransferSnapshot() const override { return transfer; }
};
#endif
}
std::unique_ptr<GSRasterBackend> CreateHardwareGSBackend() {
#ifdef PS2X_WITH_HWGS
    return std::make_unique<HardwareGS>();
#else
    throw std::runtime_error("HWGS requires a build with PS2X_WITH_HWGS=ON");
#endif
}
bool ps2HwgsSharpTake(std::vector<uint8_t>& pixels,uint32_t& width,uint32_t& height) {
    std::lock_guard<std::mutex> lock(sharpMutex);
    if(sharpPixels.empty()) return false;
    pixels=std::move(sharpPixels); sharpPixels.clear(); width=sharpWidth; height=sharpHeight; return true;
}
uint32_t ps2HwgsNativeWidth() { return nativeWidth.load(); }
int ps2HwgsPresentedScene() { return presentedScene.load(); }
void ps2HwgsNoteDirectFrame(uint32_t width,uint32_t scale,bool scene) {
    nativeWidth.store(width/std::max(1u,scale));presentedScene.store(scene?1:0);
}
