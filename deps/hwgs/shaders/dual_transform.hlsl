// SPDX-License-Identifier: GPL-3.0-or-later
// Unclipped DualTransform mesh batches. No VU bytecode or captured game data.
// Inputs: position.xyz/batch index, raw STQ/packed RGBA (32 bytes per vertex).
// Batch: two column-major matrices, STQ bias/padding (144 bytes).
ByteAddressBuffer inputs : register(t0);
ByteAddressBuffer batches : register(t1);
RWByteAddressBuffer vertices : register(u0);
cbuffer Dispatch : register(b0) { uint count; };

float normal(float value)
{
    const uint bits = asuint(value);
    return asfloat((bits & 0x7f800000u) == 0 ? bits & 0x80000000u : bits);
}

// Use double only to recover the rounding direction. A product of two normal
// binary32 inputs is exact in binary64. Integer adjustment implements RTZ;
// precise prevents contraction across the PS2's separate multiply/add steps.
float round_rtz(double value)
{
    uint bits = asuint((float)value);
    if ((bits & 0x7f800000u) == 0x7f800000u)
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    const double rounded = (double)asfloat(bits);
    if ((value > 0.0 && rounded > value) || (value < 0.0 && rounded < value))
        --bits;
    return normal(asfloat(bits));
}

float add_rtz(float a, float b)
{
    a = normal(a);
    b = normal(b);
    precise double sum = (double)a + (double)b;
    // A tiny opposite-sign operand can disappear even in binary64. RTZ still
    // requires the next binary32 value toward zero in that case.
    if (((asuint(a) ^ asuint(b)) & 0x80000000u) != 0)
    {
        if (b != 0.0f && sum == (double)a)
            return normal(asfloat(asuint(a) - 1u));
        if (a != 0.0f && sum == (double)b)
            return normal(asfloat(asuint(b) - 1u));
    }
    return round_rtz(sum);
}

float mul_rtz(float a, float b)
{
    precise double product = (double)normal(a) * (double)normal(b);
    return round_rtz(product);
}

float4 add_rtz(float4 a, float4 b)
{
    return float4(add_rtz(a.x, b.x), add_rtz(a.y, b.y), add_rtz(a.z, b.z), add_rtz(a.w, b.w));
}

float4 mul_rtz(float4 a, float b)
{
    return float4(mul_rtz(a.x, b), mul_rtz(a.y, b), mul_rtz(a.z, b), mul_rtz(a.w, b));
}

float reciprocal_rtz(float value)
{
    const uint sign = asuint(value) & 0x80000000u;
    const float magnitude = asfloat(asuint(normal(value)) & 0x7fffffffu);
    if (magnitude == 0.0f)
        return asfloat(sign | 0x7f7fffffu);
    precise float approximate = 1.0f / magnitude;
    uint bits = min(asuint(approximate), 0x7f7fffffu);
    // Direct3D reciprocal has a bounded ULP error. Refine to the greatest
    // representable magnitude whose exact product with the denominator is <= 1.
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        precise double product = (double)asfloat(bits) * (double)magnitude;
        if (product > 1.0)
            --bits;
        else if (bits < 0x7f7fffffu)
        {
            precise double next = (double)asfloat(bits + 1u) * (double)magnitude;
            if (next <= 1.0)
                ++bits;
        }
    }
    return normal(asfloat(bits | sign));
}

float4 transform(uint address, float3 p)
{
    precise float4 v = asfloat(batches.Load4(address + 48));
    v = add_rtz(v, mul_rtz(asfloat(batches.Load4(address + 32)), p.z));
    v = add_rtz(v, mul_rtz(asfloat(batches.Load4(address + 16)), p.y));
    v = add_rtz(v, mul_rtz(asfloat(batches.Load4(address)), p.x));
    return v;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count)
        return;
    const uint4 positionWords = inputs.Load4(id.x * 32);
    const uint4 attributes = inputs.Load4(id.x * 32 + 16);
    const uint batch = positionWords.w * 144;
    const float bias = asfloat(batches.Load(batch + 128));
    const float3 raw = asfloat(attributes.xyz);
    const float3 stq = float3(add_rtz(raw.x, -bias), add_rtz(raw.y, -bias), add_rtz(raw.z, -bias));
    [unroll] for (uint field = 0; field < 2; ++field)
    {
        const float4 projected = transform(batch + field * 64, asfloat(positionWords.xyz));
        const float q = reciprocal_rtz(projected.w);
        const float3 screen = mul_rtz(projected, q).xyz;
        const float3 uvq = float3(mul_rtz(stq.x, q), mul_rtz(stq.y, q), mul_rtz(stq.z, q));
        const int3 xyz = int3(screen * 16.0f);
        const uint address = (id.x * 2 + field) * 32;
        vertices.Store4(address, uint4(asuint(uvq.xy), attributes.w, asuint(uvq.z)));
        vertices.Store4(address + 16, uint4((uint(xyz.x) & 65535) | (uint(xyz.y) << 16), uint(xyz.z), 0, 0));
    }
}
