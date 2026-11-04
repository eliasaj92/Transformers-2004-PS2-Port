// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic scheduler/continuation check. No game binary or generated code.
// From an x64 MSVC developer shell, compile into a private directory with:
// cl /nologo /std:c++20 /O2 /EHsc /fp:precise /Fe:<private>/movement-check.exe
//    /Fo:<private>/movement-check.obj testing/movement_timing_check.cpp
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

struct R5900Context { uint32_t pc = 0, gpr[32]{}; float f[32]{}; };
class PS2Runtime {
public:
    using RecompiledFunction = void (*)(uint8_t*, R5900Context*, PS2Runtime*);
    std::map<uint32_t, RecompiledFunction> functions;
    bool hasFunction(uint32_t pc) const { return functions.contains(pc); }
    RecompiledFunction lookupFunction(uint32_t pc) const { return functions.at(pc); }
    void replaceFunction(uint32_t pc, RecompiledFunction fn) { functions.at(pc) = fn; }
    struct Scheduler { uint64_t currentVSyncTick() const { return 1; } } scheduler;
    Scheduler& eeScheduler() { return scheduler; }
};
static uint32_t readWord(uint8_t* ram, uint32_t address) {
    uint32_t value; std::memcpy(&value, ram + address, 4); return value;
}
#define GPR_U32(ctx, index) ((ctx)->gpr[index])
#define READ32(address) readWord(rdram, address)
#include "../PS2Recomp/ps2xRuntime/src/lib/ps2_movement_timing.h"

static constexpr uint32_t caller = 0x800000, stack = 0x900000;
static bool yieldCall = false, terminal = false;
static float start = 8;
static void fixture(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    if (yieldCall && ctx->pc == 0x476320) {
        ctx->gpr[29] -= 0x30;
        ctx->pc = 0x4a57d0; // Guest call has not returned.
        ctx->f[12] = 0.09f; // Clobber argument registers while suspended.
        return;
    }
    if (yieldCall) ctx->gpr[29] += 0x30;
    ctx->pc = caller;
    ctx->f[3] = 7; ctx->f[2] = 12; ctx->f[1] = 12 - start;
    ctx->f[0] = terminal ? 22 : start + 0.2f * ctx->f[1];
}
static void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
int main() try {
    _putenv_s("PS2X_TF_MOVE_TRACE", "");
    _putenv_s("PS2X_TF_MOVE_BLEND", "1");
    PS2Runtime rt;
    rt.functions = {{0x476320, fixture}, {0x476344, fixture}, {0x476350, fixture}};
    TfMovementTiming::install(rt, true);
    require(rt.lookupFunction(0x476344) == rt.lookupFunction(0x476320) &&
            rt.lookupFunction(0x476350) == rt.lookupFunction(0x476320), "continuations not wrapped");
    std::vector<uint8_t> ram(0x200000);
    const float coefficient = 0.2f;
    std::memcpy(ram.data() + 0x180000 - 0x75a8, &coefficient, 4);
    auto call = [&](float dt) {
        R5900Context ctx{};
        ctx.pc = 0x476320; ctx.gpr[5] = 0x100000; ctx.gpr[28] = 0x180000;
        ctx.gpr[29] = stack; ctx.gpr[31] = caller; ctx.f[12] = dt;
        rt.lookupFunction(ctx.pc)(ram.data(), &ctx, &rt);
        if (yieldCall) {
            require(TfMovementTiming::pending.size() == 1 && ctx.pc == 0x4a57d0,
                    "yield consumed the pending correction");
            ctx.pc = 0x476350;
            rt.lookupFunction(ctx.pc)(ram.data(), &ctx, &rt);
        }
        require(TfMovementTiming::pending.empty() && ctx.pc == caller && ctx.gpr[29] == stack,
                "guest return did not retire correction");
        return ctx.f[0];
    };
    const float wholeStep = 0.0333667025f;
    const float full = call(wholeStep);
    require(full == 8.8f, "reference cadence changed");
    const float half = call(wholeStep / 2);
    yieldCall = true;
    require(call(wholeStep / 2) == half, "resumed helper differs from immediate return");
    start = half;
    require(std::fabs(call(wholeStep / 2) - full) < 0.000002f,
            "two half steps differ from one whole blend step");
    start = 8;
    require(std::fabs(call(0) - start) < 0.000002f, "zero time still blends");
    terminal = true;
    require(call(wholeStep / 2) == 22, "terminal speed changed");
    std::puts("PASS: continuation, preserved inputs, blend cadence, zero delta, terminal speed");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
