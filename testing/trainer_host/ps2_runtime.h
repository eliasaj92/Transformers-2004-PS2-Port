// Minimal runtime boundary for exercising the production trainer with synthetic RAM.
#pragma once
#include <cstdint>
#include <cstring>
struct R5900Context {uint32_t pc=0;uint32_t r[32]{};float f[32]{};};
class PS2Runtime {
public:
    using RecompiledFunction=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
    bool hasFunction(uint32_t) const;
    RecompiledFunction lookupFunction(uint32_t) const;
    bool replaceFunction(uint32_t,RecompiledFunction);
};
extern const uint32_t g_ps2RecompiledFunctionTableBase,g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];
