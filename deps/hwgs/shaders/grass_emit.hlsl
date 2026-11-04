// SPDX-License-Identifier: GPL-3.0-or-later
// Grass billboard emission after CPU strip traversal and subdivision.
// One 256-byte input: clip scale/offset, billboard offset, two base/A/B
// triples, color bias/A/B and four scatter records. No VU bytecode.
// One 640-byte output: 16 XYZF2 endpoints per field, then eight raw colors.
ByteAddressBuffer inputs : register(t0);
RWByteAddressBuffer geometry : register(u0);
cbuffer Dispatch : register(b0) { uint count; };

#include "vu_math.hlsl"

float4 loadVector(uint batch, uint slot)
{
    return asfloat(inputs.Load4(batch + slot * 16));
}

float4 interpolate(uint batch, uint first, float4 weights, bool second)
{
    float4 value = add_rtz(loadVector(batch, first), mul_rtz(loadVector(batch, first + 1), weights.x));
    if (second)
        value = add_rtz(value, mul_rtz(loadVector(batch, first + 1), weights.z));
    return add_rtz(value, mul_rtz(loadVector(batch, first + 2), second ? weights.w : weights.y));
}

uint clipFlags(uint batch, float4 position)
{
    const float4 scale = loadVector(batch, 0);
    const float4 product = float4(mul_rtz(scale.x, position.x), mul_rtz(scale.y, position.y),
                                   mul_rtz(scale.z, position.z), mul_rtz(scale.w, position.w));
    const float4 clip = add_rtz(mul_rtz(loadVector(batch, 1), position.w), product);
    const float limit = abs(clip.w);
    return uint(clip.x > limit) | (uint(clip.x < -limit) << 1) |
           (uint(clip.y > limit) << 2) | (uint(clip.y < -limit) << 3) |
           (uint(clip.z > limit) << 4) | (uint(clip.z < -limit) << 5);
}

uint toFixed4(float value)
{
    // The projected value has already been rounded and flushed. Scaling a
    // normal binary32 value by 16 is exact unless it overflows; the conversion
    // below saturates that case. No second double-precision round is needed.
    precise float scaled = value * 16.0f;
    if (scaled >= 2147483648.0f)
        return 0x7fffffffu;
    if (scaled <= -2147483648.0f)
        return 0x80000000u;
    return uint(int(scaled));
}

uint4 endpoint(float4 position, float reciprocal, uint flags)
{
    const float4 projected = mul_rtz(position, reciprocal);
    return uint4(toFixed4(projected.x), toFixed4(projected.y), toFixed4(projected.z), flags);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count * 8)
        return;
    const uint batchIndex = id.x / 8, sprite = id.x % 8;
    const uint batch = batchIndex * 256, output = batchIndex * 640;
    const bool second = (sprite & 1) != 0;
    const float4 weights = loadVector(batch, 12 + sprite / 2);
    const float4 color = interpolate(batch, 9, weights, second);
    geometry.Store4(output + 512 + sprite * 16, asuint(color));
    [unroll] for (uint field = 0; field < 2; ++field)
    {
        const float4 origin = interpolate(batch, 3 + field * 3, weights, second);
        const float4 corner = add_rtz(origin, loadVector(batch, 2));
        uint flags = 0x7fffu + (clipFlags(batch, origin) & clipFlags(batch, corner));
        if ((flags & 0x8000u) != 0)
            flags |= 0xffff0000u;
        const float q = reciprocal_rtz(origin.w);
        const uint4 a = endpoint(origin, q, flags);
        const uint4 b = endpoint(corner, q, flags);
        const uint address = output + field * 256 + sprite * 32;
        geometry.Store4(address, second ? a : b);
        geometry.Store4(address + 16, second ? b : a);
    }
}
