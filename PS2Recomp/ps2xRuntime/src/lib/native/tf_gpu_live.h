// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_geometry.h"
struct VU1State;
namespace tf_gpu_live
{
inline bool enabled = false;
void install(uint32_t crc);
bool begin(uint32_t pc, bool continuing, uint32_t itop, uint64_t generation,
           const void *owner, const uint8_t *code, const uint8_t *data, const VU1State &state);
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes);
void end(bool transferred, bool fellBack);
void finish();
}
