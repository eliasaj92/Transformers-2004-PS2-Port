// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keep the small normalization/arithmetic helpers inside their strict-FP
// translation units. Conservative release inlining otherwise leaves a call
// around nearly every emulated scalar operation. This changes call overhead,
// not rounding mode, operation order, denormal handling or non-finite clamping.
#if defined(_MSC_VER)
#define GS_NATIVE_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define GS_NATIVE_FORCEINLINE inline __attribute__((always_inline))
#else
#define GS_NATIVE_FORCEINLINE inline
#endif
