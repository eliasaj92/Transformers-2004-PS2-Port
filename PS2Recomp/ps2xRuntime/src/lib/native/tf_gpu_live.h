// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "runtime/gs/gs_native_geometry.h"
struct VU1State;
class PS2Memory;
class GS;
namespace tf_gpu_live
{
inline bool enabled = false;
inline bool bypassEnabled = false;
void install(uint32_t crc);
bool begin(uint32_t pc, bool continuing, uint32_t itop, uint64_t generation, const void *owner, const uint8_t *code,
           const uint8_t *data, const VU1State &state);
gsnative::DualPacketPtr packet(uint32_t path, const uint8_t *data, uint32_t bytes);
bool suppressPacket();
bool prepareSetup(uint32_t entry, bool continuing, const void *owner, const uint8_t *code, uint8_t *data);
bool tryTail(uint8_t *code, uint8_t *data, VU1State &state, GS &gs, PS2Memory *memory, uint32_t entry, uint32_t top,
             uint32_t itop, uint32_t budget, void *transfer);
// Caller must hold exclusive VU-memory ownership, or fence the worker first.
enum ReadReason : unsigned
{
    EeRead,
    InputRead,
    UnpackRead,
    ProgramRead
};
void readMemory(const void *owner, uint8_t *data, unsigned first = 0, unsigned count = 1024,
                ReadReason reason = EeRead);
void unpackMemory(const void *owner, uint8_t *data, unsigned address, unsigned count, unsigned cl, unsigned wl,
                  bool overwritesXYZ);
void resetMemory(const void *owner);
void end(bool transferred, bool fellBack);
void finish();
} // namespace tf_gpu_live
