// SPDX-License-Identifier: GPL-3.0-or-later
// Lighting/reflection arithmetic. Each source record starts with eleven
// constant vectors followed by STQ, normal and object-space position.
// TF_GPU_REFLECTION_PROJECT appends two matrices, draw opacity and restart
// flags, then emits four native vertex views plus diagnostic packed GIF words.
ByteAddressBuffer inputs : register(t0);
RWByteAddressBuffer geometry : register(u0);
cbuffer Dispatch : register(b0) { uint count; };
#include "vu_math.hlsl"
#ifdef TF_GPU_REFLECTION_CULL
// One packet per group (at most 22 source vertices). Shared results avoid
// repeating lighting/projection when expanding the strip into culled triangles.
groupshared uint4 nativeVertices[22 * 8];
#endif

precise float4 loadVector(uint base, uint slot) { return asfloat(inputs.Load4(base + slot * 16)); }
precise float3 times(float3 a, float3 b)
{
    return float3(mul_rtz(a.x, b.x), mul_rtz(a.y, b.y), mul_rtz(a.z, b.z));
}
precise float3 scale(float3 a, float b) { return times(a, b.xxx); }
precise float3 plus(float3 a, float3 b)
{
    return float3(add_rtz(a.x, b.x), add_rtz(a.y, b.y), add_rtz(a.z, b.z));
}
precise float sum3(float3 a) { return add_rtz(add_rtz(a.x, a.y), a.z); }
precise float sqrt_rtz(float value)
{
    if (value == 0.0f) return 0.0f;
    precise float approximate = sqrt(value);
    uint bits = asuint(approximate);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        precise double square = (double)asfloat(bits) * (double)asfloat(bits);
        precise double next = (double)asfloat(bits + 1u) * (double)asfloat(bits + 1u);
        if (square > (double)value) --bits;
        else if (next <= (double)value) ++bits;
    }
    return normal(asfloat(bits));
}
uint integer_rtz(float value)
{
    if (value >= 2147483648.0f) return 0x7fffffffu;
    if (value <= -2147483648.0f) return 0x80000000u;
    return asuint((int)value);
}

void calculate(uint3 id)
{
    if (id.x >= count) return;
#ifdef TF_GPU_REFLECTION_PROJECT
    const uint base = id.x * 368;
#else
    const uint base = id.x * 224, output = id.x * 48;
#endif
    const precise float4 stq = loadVector(base, 11), sourceNormal = loadVector(base, 12);
    const precise float4 position = loadVector(base, 13);
    const precise float3 n = sourceNormal.xyz;
    const precise float4 light0 = loadVector(base, 5), light1 = loadVector(base, 9);
    const precise float3 color0 = loadVector(base, 6).xyz, color1 = loadVector(base, 10).xyz;
    const precise float a = max(sum3(times(n, light0.xyz)), 0.0f);
    const precise float b = max(sum3(times(n, light1.xyz)), 0.0f);
    const precise float3 diffuse = plus(scale(color1, b), scale(color0, a));
    const precise float3 illumination = plus(scale(diffuse, light0.w), light1.www);
    const precise float3 material = times(loadVector(base, 7).xyz, color0);
    const precise float3 rgb = max(min(scale(times(material, illumination), 128.0f), 255.0f), 0.0f);

    const precise float3 eye = plus(loadVector(base, 4).xyz, -position.xyz);
    const precise float lengthSquared = sum3(times(eye, eye));
    const precise float3 view = scale(eye, reciprocal_rtz(sqrt_rtz(abs(lengthSquared))));
    const precise float facing = sum3(times(n, view));
    const precise float3 reflected = plus(scale(plus(n, n), facing), -view);
    const precise float3 transformed = plus(plus(scale(loadVector(base, 0).xyz, reflected.x),
        scale(loadVector(base, 1).xyz, reflected.y)), scale(loadVector(base, 2).xyz, reflected.z));
    const precise float reflectionLength = sqrt_rtz(sum3(times(transformed, transformed)));
    const precise float inverseLength = reflectionLength == 0.0f ? reflectionLength : reciprocal_rtz(reflectionLength);
    const precise float3 direction = scale(transformed, inverseLength);
    const precise float denominator = add_rtz(direction.z, 1.0f);
    const precise float q = mul_rtz(0.5f, reciprocal_rtz(denominator));
    const precise float2 uv = float2(add_rtz(mul_rtz(direction.x, q), 0.5f),
                            add_rtz(mul_rtz(direction.y, q), 0.5f));
    const precise float4 alpha = loadVector(base, 8);
    const precise float opacity = mul_rtz(mul_rtz(255.0f, alpha.x), mul_rtz(add_rtz(1.0f, -facing), alpha.y));
    const uint4 lit = uint4(uint3(rgb), integer_rtz(opacity));
#ifdef TF_GPU_REFLECTION_PROJECT
    const uint drawAlpha = (uint)mul_rtz(asfloat(inputs.Load(base + 352)), 128.0f);
    const uint flags = inputs.Load(base + 356);
    const uint reflectedColor = (lit.w & 0x80000000u) != 0 ? 0 : lit.w;
    [unroll] for (uint field = 0; field < 2; ++field)
    {
        const uint matrix = base + 224 + field * 64;
        precise float4 projected = asfloat(inputs.Load4(matrix + 48));
        projected = add_rtz(projected, mul_rtz(asfloat(inputs.Load4(matrix + 32)), position.z));
        projected = add_rtz(projected, mul_rtz(asfloat(inputs.Load4(matrix + 16)), position.y));
        projected = add_rtz(projected, mul_rtz(asfloat(inputs.Load4(matrix)), position.x));
        const precise float inverseW = reciprocal_rtz(projected.w);
        const precise float3 screen = mul_rtz(projected, inverseW).xyz;
        const int3 xyz = int3(screen * 16.0f);
        [unroll] for (uint pass = 0; pass < 2; ++pass)
        {
            const uint section = pass * 2 + field;
            const precise float3 tex = pass == 0 ? float3(uv, denominator) : float3(stq.xy, 1.0f);
            const precise float3 uvq = scale(tex, inverseW);
            const uint4 color = uint4(pass == 0 ? reflectedColor.xxx : lit.xyz, drawAlpha);
            const uint rgba = (color.x & 255) | ((color.y & 255) << 8) | ((color.z & 255) << 16) |
                              ((color.w & 255) << 24);
            const uint address = (id.x * 4 + section) * 32;
            const uint4 first = uint4(asuint(uvq.xy), rgba, asuint(uvq.z) == 0 ? 0x00800000u : asuint(uvq.z));
            const uint4 second = uint4((uint(xyz.x) & 65535) | (uint(xyz.y) << 16),
                (uint(xyz.z) >> 4) & 0x00ffffffu, 0, (flags >> 4) & 255);
#ifdef TF_GPU_REFLECTION_CULL
            nativeVertices[address / 16] = first;
            nativeVertices[address / 16 + 1] = second;
#else
            geometry.Store4(address, first);
            geometry.Store4(address + 16, second);
#endif
#ifdef TF_GPU_REFLECTION_CAPTURE
            // Offline diagnostic readback only. Resident draws compile this
            // out; unused packed STQ.w has no GS vertex semantics.
            const uint rawAddress = count * 128 + (id.x * 4 + section) * 48;
            geometry.Store4(rawAddress, uint4(asuint(uvq), 0));
            geometry.Store4(rawAddress + 16, color);
            geometry.Store4(rawAddress + 32, uint4(uint3(xyz), (flags & 32768) != 0 ? flags | 0xffff0000u : flags));
#endif
        }
    }
#else
    geometry.Store4(output, asuint(float4(uv, denominator, stq.y)));
    geometry.Store4(output + 16, lit);
    geometry.Store4(output + 32, asuint(float4(position.xyz, stq.x)));
#endif
}

#ifdef TF_GPU_REFLECTION_CULL
bool culled(uint vertex, uint section)
{
    const int4 scissor = asint(inputs.Load4(count * 368 + section * 16));
    const int2 offset = asint(inputs.Load2(count * 368 + 64 + section * 16));
    int2 points[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        const uint xy = nativeVertices[(vertex + corner) * 8 + section * 2 + 1].x;
        points[corner] = int2(xy & 65535, xy >> 16) - offset;
    }
    int4 box = int4(min(min(points[0], points[1]), points[2]), max(max(points[0], points[1]), points[2]));
    box.zw -= int2((box.zw & 15) == 0);
    return any(box.xy >= box.zw) || any(box.zw + 1 <= scissor.xy) || any(box.xy >= scissor.zw) ||
        all(points[0] == points[1]) || all(points[1] == points[2]) || all(points[0] == points[2]);
}
#endif

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    calculate(id);
#ifdef TF_GPU_REFLECTION_CULL
    // Every lane reaches the barrier, including the lanes without a source.
    GroupMemoryBarrierWithGroupSync();
    if (id.x + 2 >= count) return;
    [unroll] for (uint section = 0; section < 4; ++section)
    {
        const bool skip = culled(id.x, section);
        [unroll] for (uint corner = 0; corner < 3; ++corner)
        {
            const uint input = (id.x + corner) * 8 + section * 2;
            const uint address = ((id.x * 3 + corner) * 4 + section) * 32;
            uint4 second = nativeVertices[input + 1];
            if (skip) second.xy = nativeVertices[id.x * 8 + section * 2 + 1].xy;
            geometry.Store4(address, nativeVertices[input]);
            geometry.Store4(address + 16, second);
        }
    }
#endif
}
