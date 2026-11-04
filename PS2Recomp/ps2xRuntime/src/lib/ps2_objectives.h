// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
class PS2Runtime;
namespace ps2_objectives {
void install(PS2Runtime&, uint32_t crc);
void sample(const uint8_t* ram);
void draw(float x, float y, float width, float height);
}
