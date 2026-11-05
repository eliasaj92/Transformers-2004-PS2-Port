// SPDX-License-Identifier: GPL-3.0-or-later
#include "tf_native_probe.h"
#include "tf_scene_capture.h"
#include "probe_scene.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#include "GS/Renderers/DX12/GSTexture12.h"
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tf_native_probe {
bool requested() {
    const char* output = std::getenv("PS2X_TF_NATIVE_HOST_PROBE");
    return output && *output;
}
void enableHostValidation() {
    // Standard D3D12 validation, matching the standalone probe. Do not enable
    // PCSX2's GPU-instrumented shader mode for every legacy draw in the level.
    wil::com_ptr_nothrow<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put()))))
        throw std::runtime_error("D3D12 debug layer unavailable for native host probe");
    debug->EnableDebugLayer();
}
namespace {
std::filesystem::path path(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) throw std::runtime_error(std::string("Missing ") + name);
    auto result = std::filesystem::u8path(value);
    if (!result.is_absolute()) throw std::runtime_error(std::string("Expected absolute path in ") + name);
    return result;
}
struct State {
    bool enabled = requested(), attempted = false, complete = false, resumed = false;
    uint64_t firstTick = 0;
    std::filesystem::path output;
};
void capture(GSDevice12& device, GSTexture12& current, const TfGpuFrame& frame,
             const std::filesystem::path& prefix) {
    const auto value = prefix.string();
    tf_capture::SceneCapture shot(value.c_str(), frame.tick, frame.tick);
    shot.record(device, current, frame);
    if (!device.ExecuteCommandList(GSDevice12::WaitType::None))
        throw std::runtime_error("Cannot submit host validation capture");
    shot.finish(device); // diagnostic only: submit, wait, read back once
}
}
void afterSubmit(GSDevice12& device, GSTexture12& current, const TfGpuFrame& frame) {
    static State state;
    if (!state.enabled || state.resumed) return;
    if (state.complete) {
        if (frame.tick < state.firstTick + 60) return;
        capture(device, current, frame, state.output / "host-resumed");
        state.resumed = true;
        std::ofstream record(state.output / "host.json");
        record << "{\"shared_device\":true,\"shared_queue\":true,\"first_tick\":" << state.firstTick
               << ",\"resumed_tick\":" << frame.tick << ",\"game_width\":" << frame.width
               << ",\"game_height\":" << frame.height
               << ",\"scope\":\"Diagnostic native pass inside the enhanced game; no level-mesh replacement or FPS claim\"}\n";
        if (!record) throw std::runtime_error("Cannot write native host-probe result");
        std::fprintf(stderr, "[native-host-probe] resumed tick=%llu after=%llu; shared device/queue; not a benchmark\n",
                     frame.tick, state.firstTick);
        return;
    }
    if (state.attempted) return;
    // The external route runner creates this file only after the stable
    // player_ready marker. No fixed startup/menu field number is assumed.
    const auto arm = path("PS2X_TF_NATIVE_HOST_ARM");
    if (!std::filesystem::is_regular_file(arm)) return;
    state.attempted = true; // never retry a failed probe in the same game run
    state.firstTick = frame.tick;
    state.output = path("PS2X_TF_NATIVE_HOST_PROBE");
    if (std::filesystem::exists(state.output)) throw std::runtime_error("Native host probe requires a fresh output directory");
    std::filesystem::create_directories(state.output);
    const auto asset = path("PS2X_TF_NATIVE_HOST_ASSET");
    const auto shader = path("PS2X_TF_NATIVE_HOST_SHADER");
    std::fprintf(stderr, "[native-host-probe] begin tick=%llu game=%ux%u; shared device/queue; diagnostic readbacks\n",
                 frame.tick, frame.width, frame.height);
    capture(device, current, frame, state.output / "host-before");
    {
        tfnative::Renderer native(1920, 1080, device.GetDevice(), device.GetCommandQueue(), shader);
        if (!native.validationEnabled()) throw std::runtime_error("Host probe requires the D3D12 debug layer");
        tfnative::runMeshProbe(native, asset, state.output / "native");
    }
    // Pixel equality proves the separate native target did not overwrite or
    // corrupt the game's completed output while sharing its device and queue.
    capture(device, current, frame, state.output / "host-after");
    state.complete = true;
    device.InvalidateCachedState();
    std::fprintf(stderr, "[native-host-probe] native pass complete tick=%llu; awaiting gameplay continuation\n", frame.tick);
}
}
