// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
class PS2Runtime;
struct VU1State;

// Observation only: no guest writes, replacement geometry, or added VU fences.
// Install before guest execution. The flag is immutable while workers run.
namespace ps2_native_seam_probe {
extern bool enabled;
extern bool captureEnabled;
void install(PS2Runtime& runtime, uint32_t crc);
void unpack(uint32_t command, uint32_t destination, const uint8_t* payload,
            uint32_t bytes, uint32_t cycle, uint32_t mode);
void launch(uint32_t opcode, uint32_t pc, uint32_t top, uint32_t itop,
            const uint8_t* code);
void upload(uint32_t destination, const uint8_t* payload, uint32_t bytes);
// Called on the executing VU thread, around the accelerated backend only.
// The returned ticket binds ordered GIF packets to this exact invocation.
uint32_t vuBegin(uint32_t pc, bool continuing, uint32_t top, uint32_t itop,
                 const uint8_t* code, const uint8_t* data, const VU1State& state);
void vuEnd(uint32_t ticket, const uint8_t* code, const uint8_t* data,
           const VU1State& state, bool needsTransfer, bool fellBack);
void gif(uint32_t path, const uint8_t* data, uint32_t bytes,
         bool drainImmediately, bool path2DirectHl);
}
