#pragma once
#include <cstdint>
struct Texture;
bool ps2HwgsDirectEnabled();
void ps2HwgsDirectInitialize();
void ps2HwgsDirectUpdate(Texture& texture,uint32_t& width,uint32_t& height);
void ps2HwgsDirectShutdown();
// Metadata shared by the private hardware backend and host presenter.
void ps2HwgsNoteDirectFrame(uint32_t width,uint32_t scale,bool scene);
