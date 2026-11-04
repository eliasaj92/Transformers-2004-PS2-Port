// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_native_dual_math.h"
#include <bit>
#include <cstring>
#include <stdexcept>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace gsnative
{
DualFloatScope::DualFloatScope()
{
#if defined(_M_X64) || defined(__x86_64__)
    csr = _mm_getcsr();
    savedOk = std::fegetenv(&saved) == 0;
    ready = savedOk && std::fesetround(FE_TOWARDZERO) == 0;
    if (ready)
        _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
}
DualFloatScope::~DualFloatScope()
{
    if (savedOk)
        std::fesetenv(&saved);
#if defined(_M_X64) || defined(__x86_64__)
    _mm_setcsr(csr);
#endif
}
namespace
{
float normal(float value)
{
    uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = bits & 0x7f800000u;
    if (!exponent)
        bits &= 0x80000000u;
    else if (exponent == 0x7f800000u)
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    return std::bit_cast<float>(bits);
}
float load(const uint8_t *p)
{
    float value;
    std::memcpy(&value, p, 4);
    return normal(value);
}
float add(float a, float b)
{
    return normal(normal(a) + normal(b));
}
float multiply(float a, float b)
{
    return normal(normal(a) * normal(b));
}
uint32_t fixed4(float value)
{
    value *= 16.0f;
    if (value >= 2147483648.0f)
        return 0x7fffffffu;
    if (value <= -2147483648.0f)
        return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(value));
}
} // namespace
DualRawVertex dualRawVertex(const DualPacket &packet, unsigned vertex, unsigned field)
{
    const uint8_t *input = packet.inputs.data() + vertex * 32;
    const uint8_t *matrix = packet.constants.data() + field * 64;
    const float x = load(input), y = load(input + 4), z = load(input + 8);
    float projected[4];
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        float v = load(matrix + 48 + lane * 4);
        v = add(v, multiply(load(matrix + 32 + lane * 4), z));
        v = add(v, multiply(load(matrix + 16 + lane * 4), y));
        projected[lane] = add(v, multiply(load(matrix + lane * 4), x));
    }
    const float q = projected[3] == 0.0f
                        ? std::bit_cast<float>((std::bit_cast<uint32_t>(projected[3]) & 0x80000000u) | 0x7f7fffffu)
                        : normal(1.0f / projected[3]);
    const float bias = load(packet.constants.data() + 128);
    DualRawVertex result;
    for (unsigned lane = 0; lane < 3; ++lane)
    {
        result.stq[lane] = std::bit_cast<uint32_t>(multiply(add(load(input + 16 + lane * 4), -bias), q));
        result.xyz[lane] = fixed4(multiply(projected[lane], q));
    }
    return result;
}
void materializeDualPacket(const DualPacket &packet, uint8_t *gif, uint32_t bytes)
{
    if (packet.version != 2)
        return;
    if (!gif || packet.vertices < 3 || packet.vertices > 28 || bytes != (8 + 6 * packet.vertices) * 16)
        throw std::runtime_error("Malformed deferred geometry packet");
    DualFloatScope floating;
    if (!floating.ready)
        throw std::runtime_error("Cannot materialize deferred geometry in the required FP mode");
    for (unsigned field = 0; field < 2; ++field)
        for (unsigned vertex = 0; vertex < packet.vertices; ++vertex)
        {
            const auto raw = dualRawVertex(packet, vertex, field);
            auto *p = gif + field * (4 + 3 * packet.vertices) * 16 + 64 + vertex * 48;
            std::memcpy(p, raw.stq.data(), 12);
            std::memcpy(p + 32, raw.xyz.data(), 12);
        }
}
} // namespace gsnative
