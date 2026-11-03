#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
class PS2Runtime;
struct R5900Context;
namespace ps2x::trainer {
enum class Unlock { MiniCons = 1, Levels = 2 };
void install(PS2Runtime& runtime, uint32_t elfCrc32);
void service(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
std::filesystem::path selectSaveRoot(const std::filesystem::path& normalRoot);
std::string status();
bool canUnlock();
bool requestUnlock(Unlock action);
}
