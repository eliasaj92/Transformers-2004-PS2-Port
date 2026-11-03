#pragma once
#include <cstdint>
#include <string>
// Read-only qualification output. Called on the EE thread by the script harness.
namespace ps2x::trainer { std::string diagnostics(const uint8_t* ram); }
