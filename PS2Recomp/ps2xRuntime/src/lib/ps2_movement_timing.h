// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Private USA timing qualification; included after runtime and guest macros.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace TfMovementTiming {
inline PS2Runtime::RecompiledFunction original = nullptr;
inline FILE* trace = nullptr;
inline bool correctBlend = false;
inline uint64_t rows = 0;
struct Call {
    uint32_t caller, stack, actor;
    float delta, target, previous, coefficient;
};
// EE-thread state. The helper's two guest calls can yield and resume through
// registered continuation entries before the guest return restores its stack.
inline std::vector<Call> pending;

inline float readFloat(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t address) {
    const uint32_t bits = READ32(address);
    float value; std::memcpy(&value, &bits, 4); return value;
}

inline void finish(const Call& call, R5900Context* ctx, PS2Runtime* runtime) {
    const float dt = call.delta, coefficient = call.coefficient;
    const float observed = ctx->f[0];
    // The helper retains WalkMax in f3, RunMin in f2 and the signed blend gap
    // in f1. Only its gap-crossing branches return strictly between the limits.
    const bool blended = observed > ctx->f[3] && observed < ctx->f[2];
    float effective = coefficient;
    constexpr double originalStep = 0.0333667025;
    bool changed = false;
    if (correctBlend && blended) {
        if (!(std::isfinite(dt) && dt >= 0 && dt <= 0.1f && coefficient > 0 && coefficient < 1 &&
              std::isfinite(ctx->f[1])))
            throw std::runtime_error("Movement timing qualification encountered an unsupported blend");
        if (std::fabs(double(dt)-originalStep) > 0.0000001) {
            effective = static_cast<float>(-std::expm1(std::log1p(-double(coefficient))*double(dt)/originalStep));
            ctx->f[0] = observed + (effective-coefficient)*ctx->f[1];
            changed = true;
        }
    }
    if (trace && rows++ < 100000) {
        std::fprintf(trace, "%llu,%08x,%08x,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d\n",
            static_cast<unsigned long long>(runtime->eeScheduler().currentVSyncTick()), call.caller, call.actor,
            dt, call.target, call.previous, ctx->f[3], ctx->f[2], ctx->f[1], coefficient, effective,
            observed, ctx->f[0], blended, changed);
        std::fflush(trace);
    }
}

inline void speed(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t actor = GPR_U32(ctx, 5), gp = GPR_U32(ctx, 28);
    if (ctx->pc == 0x00476320u && actor >= 0x100000u && actor <= 0x2000000u-0x300 &&
        gp >= 0x75a8 && gp < 0x2000000u) {
        if (pending.size() >= 64)
            throw std::runtime_error("Movement timing qualification exceeded pending call limit");
        pending.push_back({GPR_U32(ctx, 31), GPR_U32(ctx, 29), actor, ctx->f[12], ctx->f[13],
                           readFloat(rdram, ctx, runtime, actor+0x44), readFloat(rdram, ctx, runtime, gp-0x75a8)});
    }
    original(rdram, ctx, runtime);
    for (size_t i = pending.size(); i-- > 0;) {
        const auto call = pending[i];
        if (ctx->pc != call.caller || GPR_U32(ctx, 29) != call.stack) continue;
        finish(call, ctx, runtime);
        pending.erase(pending.begin() + i);
    }
}

inline void install(PS2Runtime& rt, bool fullRate) {
    const char* path = std::getenv("PS2X_TF_MOVE_TRACE");
    const char* option = std::getenv("PS2X_TF_MOVE_BLEND");
    correctBlend = option && std::strcmp(option, "1") == 0;
    if ((!path || !*path) && !correctBlend) return;
    if (original) throw std::runtime_error("Movement timing qualification already installed");
    if (correctBlend && !fullRate)
        throw std::runtime_error("PS2X_TF_MOVE_BLEND requires native simulation timing variant 3");
    if (!rt.hasFunction(0x00476320u)) throw std::runtime_error("Missing robot speed helper");
    if (path && *path) {
        trace = std::fopen(path, "wx");
        if (!trace) throw std::runtime_error("Use a fresh private movement trace path");
        std::fprintf(trace, "vs,caller,context,delta,target,previous,walk_max,run_min,gap,coefficient,effective,original_result,result,blended,changed\n");
    }
    original = rt.lookupFunction(0x00476320u);
    pending.reserve(64);
    for (uint32_t pc = 0x00476320u; pc < 0x00476460u; pc += 4)
        if (rt.hasFunction(pc) && rt.lookupFunction(pc) == original)
            rt.replaceFunction(pc, &speed);
    std::fprintf(stderr, "[movement-timing] robot speed trace=%d time-scaled blend=%d (private qualification)\n", trace != nullptr, correctBlend);
}
}
