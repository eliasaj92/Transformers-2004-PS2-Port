#pragma once
#include "runtime/gs/gs_backend.h"
#include <memory>

std::unique_ptr<GSRasterBackend> CreateHardwareGSBackend();
bool ps2HwgsSharpTake(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height);
uint32_t ps2HwgsNativeWidth();
int ps2HwgsPresentedScene();
