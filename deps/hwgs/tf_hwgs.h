#pragma once
#include <stdint.h>

#ifdef TF_HWGS_BUILD
#define TF_HWGS_API __declspec(dllexport)
#else
#define TF_HWGS_API __declspec(dllimport)
#endif

// The DLL keeps PCSX2's globals, GS types, fmt and ImGui separate from PS2Recomp.
// All calls come from one ordered GS producer; PCSX2 owns its internal worker.
extern "C" {
TF_HWGS_API int tf_gs_open(int scale, int fxaa, const char* resources, char* error, uint32_t capacity);
TF_HWGS_API void tf_gs_close();
TF_HWGS_API void tf_gs_reset();
TF_HWGS_API void tf_gs_sync();
TF_HWGS_API void tf_gs_configure(int scale, int fxaa);
TF_HWGS_API void tf_gs_gif(uint32_t path, const uint8_t* data, uint32_t bytes);
TF_HWGS_API void tf_gs_vsync(const uint8_t* privilegedRegisters);
// Returns required bytes. Copy only when capacity is sufficient; dimensions always returned.
TF_HWGS_API uint32_t tf_gs_frame(uint8_t* data, uint32_t capacity, uint32_t* width, uint32_t* height);
// Rectangle readback does not disturb an in-progress GIF transfer or guest registers.
TF_HWGS_API uint32_t tf_gs_read_rect(uint32_t psm, uint32_t base, uint32_t bw,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t* data, uint32_t capacity);
TF_HWGS_API uint32_t tf_gs_read_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
TF_HWGS_API void tf_gs_write_pixel(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
TF_HWGS_API void tf_gs_snapshot(uint8_t* data, uint32_t capacity);
TF_HWGS_API void tf_gs_upload_vram(const uint8_t* data, uint32_t bytes);
}
