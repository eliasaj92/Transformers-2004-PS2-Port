// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Opt-in live comparison. Always executes the accelerated reference unchanged.
namespace tf_grass_oracle {
extern bool enabled; // Immutable while guest workers run.
void install(uint32_t crc);
bool begin(uint32_t pc, bool continuing, uint32_t itop, const uint8_t* code, const uint8_t* data);
void gif(uint32_t path, const uint8_t* data, uint32_t bytes);
void end(bool needsTransfer, bool fellBack);
void finish(); // Explicit report: the runner exits without static destructors.
}
