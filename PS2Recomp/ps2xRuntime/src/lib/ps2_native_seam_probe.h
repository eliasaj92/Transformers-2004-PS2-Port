// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
class PS2Runtime;

// Observation only: no guest writes, replacement geometry, or added VU fences.
// Install before guest execution. The flag is immutable while workers run.
namespace ps2_native_seam_probe {
extern bool enabled;
void install(PS2Runtime& runtime, uint32_t crc);
void unpack(uint32_t command, uint32_t destination, const uint8_t* payload,
            uint32_t bytes, uint32_t cycle, uint32_t mode);
void launch(uint32_t opcode, uint32_t pc, uint32_t top, uint32_t itop,
            const uint8_t* code);
void upload(uint32_t destination, const uint8_t* payload, uint32_t bytes);
}
