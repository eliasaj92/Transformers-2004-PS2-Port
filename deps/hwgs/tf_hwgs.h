#pragma once
#include <stdint.h>
#include "runtime/gs/gs_native_geometry.h"

#ifdef TF_HWGS_BUILD
#define TF_HWGS_API __declspec(dllexport)
#else
#define TF_HWGS_API __declspec(dllimport)
#endif

// The DLL keeps PCSX2's globals, GS types, fmt and ImGui separate from PS2Recomp.
// Rendering calls come from one ordered GS producer; PCSX2 owns its worker.
// Direct frame acquire/release calls come from the host presentation thread.
extern "C" {
TF_HWGS_API int tf_gs_open(int scale, int fxaa, const char* resources, char* error, uint32_t capacity);
TF_HWGS_API void tf_gs_close();
TF_HWGS_API void tf_gs_reset();
TF_HWGS_API void tf_gs_sync();
TF_HWGS_API void tf_gs_configure(int scale, int fxaa);
TF_HWGS_API void tf_gs_gif(uint32_t path, const uint8_t* data, uint32_t bytes);
// Copies the companion and fallback packet into the same ordered GS queue.
// Returns zero without enqueuing when unsupported. No per-draw completion wait.
TF_HWGS_API int tf_gs_native_dual_gif(uint32_t path, const uint8_t* data, uint32_t bytes,
                                    const gsnative::DualPacket* geometry);
TF_HWGS_API void tf_gs_vsync(const uint8_t* privilegedRegisters);
// Returns required bytes. Copy only when capacity is sufficient; dimensions always returned.
TF_HWGS_API uint32_t tf_gs_frame(uint8_t* data, uint32_t capacity, uint32_t* width, uint32_t* height);
// Windows GPU-resident presentation. Handles remain owned by the DLL, and are
// valid while this slot is acquired. OpenGL imports do not consume NT handles.
struct TfGpuFrame {
    uint32_t slot, generation, width, height, texture_width, texture_height, scale, scene;
    uint32_t display_fbp;
    uint64_t sequence, tick, ready_value, release_value;
    void* memory_handle;
    void* fence_handle;
    uint8_t adapter_luid[8];
};
TF_HWGS_API int tf_gs_direct_enabled();
TF_HWGS_API int tf_gs_direct_publish(uint32_t scale, uint32_t scene, uint32_t display_fbp, uint64_t tick);
TF_HWGS_API int tf_gs_direct_acquire(TfGpuFrame* frame);
// Call only after submitting/flushing the OpenGL release signal.
TF_HWGS_API void tf_gs_direct_release(uint32_t slot, uint32_t generation);
TF_HWGS_API void tf_gs_direct_close();
// Rectangle readback does not disturb an in-progress GIF transfer or guest registers.
TF_HWGS_API uint32_t tf_gs_read_rect(uint32_t psm, uint32_t base, uint32_t bw,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t* data, uint32_t capacity);
TF_HWGS_API uint32_t tf_gs_read_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
TF_HWGS_API void tf_gs_write_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
TF_HWGS_API void tf_gs_snapshot(uint8_t* data, uint32_t capacity);
TF_HWGS_API void tf_gs_upload_vram(const uint8_t* data, uint32_t bytes);
}
