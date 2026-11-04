// SPDX-License-Identifier: GPL-3.0-or-later
// Unclipped DualTransform mesh batches. No VU bytecode or captured game data.
// Inputs: position.xyz/batch index, raw STQ/packed RGBA (32 bytes per vertex).
// Batch: two column-major matrices, STQ bias, two restart masks and the first
// source vertex index (144 bytes). Output uses PACKED XYZF2 depth/fog semantics.
#ifdef TF_GPU_DUAL_GROUPED
// Up to eight independent resident source buffers. Grouped transform records
// have a 160-byte stride: the ordinary 144 bytes, source count and output base.
ByteAddressBuffer source0 : register(t0);
ByteAddressBuffer source1 : register(t1);
ByteAddressBuffer source2 : register(t2);
ByteAddressBuffer source3 : register(t3);
ByteAddressBuffer source4 : register(t4);
ByteAddressBuffer source5 : register(t5);
ByteAddressBuffer source6 : register(t6);
ByteAddressBuffer source7 : register(t7);
ByteAddressBuffer batches : register(t8);
uint4 sourceWords(uint slot, uint address)
{
    switch (slot)
    {
    case 0: return source0.Load4(address);
    case 1: return source1.Load4(address);
    case 2: return source2.Load4(address);
    case 3: return source3.Load4(address);
    case 4: return source4.Load4(address);
    case 5: return source5.Load4(address);
    case 6: return source6.Load4(address);
    default: return source7.Load4(address);
    }
}
#ifdef TF_GPU_DUAL_CAPTURE
#error Grouped draws use the native frontend diagnostic, not the raw capture layout.
#endif
#else
ByteAddressBuffer inputs : register(t0);
ByteAddressBuffer batches : register(t1);
#endif
RWByteAddressBuffer vertices : register(u0);
cbuffer Dispatch : register(b0) { uint count; };

#include "vu_math.hlsl"

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
#ifdef TF_GPU_DUAL_GROUPED
    const uint slot = id.x / 32, vertex = id.x % 32;
    if (slot >= count)
        return;
    const uint batch = slot * 160;
    if (vertex >= batches.Load(batch + 144))
        return;
    const uint4 positionWords = sourceWords(slot, vertex * 32);
    const uint4 attributes = sourceWords(slot, vertex * 32 + 16);
    const uint outputVertex = batches.Load(batch + 148) + vertex;
#else
    if (id.x >= count)
        return;
    const uint4 positionWords = inputs.Load4(id.x * 32);
    const uint4 attributes = inputs.Load4(id.x * 32 + 16);
    const uint batch = positionWords.w * 144;
    const uint vertex = id.x - batches.Load(batch + 140);
    const uint outputVertex = id.x;
#endif
    const float bias = asfloat(batches.Load(batch + 128));
    const uint mask = batches.Load(batch + 132 + (vertex / 16) * 4);
    const uint flags = (mask << (vertex % 16)) & 65535;
    const float3 raw = asfloat(attributes.xyz);
    const float3 stq = float3(add_rtz(raw.x, -bias), add_rtz(raw.y, -bias), add_rtz(raw.z, -bias));
    bool sameTransform = true;
    [unroll] for (uint column = 0; column < 4; ++column)
        sameTransform = sameTransform && all(batches.Load4(batch + column * 16) ==
                                             batches.Load4(batch + 64 + column * 16));
    uint4 previousLow = 0, previousHigh = 0;
#ifdef TF_GPU_DUAL_CAPTURE
    uint4 previousRawLow = 0, previousRawHigh = 0;
#endif
    [unroll] for (uint field = 0; field < 2; ++field)
    {
        const uint address = (outputVertex * 2 + field) * 32;
        // Exact constant equality permits reusing the first field's result.
        // GS SCISSOR/XYOFFSET remain separate per-field draw state.
        [branch] if (field != 0 && sameTransform)
        {
            vertices.Store4(address, previousLow);
            vertices.Store4(address + 16, previousHigh);
#ifdef TF_GPU_DUAL_CAPTURE
            vertices.Store4(count * 64 + address, previousRawLow);
            vertices.Store4(count * 64 + address + 16, previousRawHigh);
#endif
            continue;
        }
        const float4 projected = transform(batch + field * 64, asfloat(positionWords.xyz));
        const float q = reciprocal_rtz(projected.w);
        const float3 screen = mul_rtz(projected, q).xyz;
        const float3 uvq = float3(mul_rtz(stq.x, q), mul_rtz(stq.y, q), mul_rtz(stq.z, q));
        const int3 xyz = int3(screen * 16.0f);
        const uint qword = asuint(uvq.z) == 0 ? 0x00800000u : asuint(uvq.z);
        previousLow = uint4(asuint(uvq.xy), attributes.w, qword);
        previousHigh = uint4((uint(xyz.x) & 65535) | (uint(xyz.y) << 16),
            (uint(xyz.z) >> 4) & 0x00ffffffu, 0, (flags >> 4) & 255);
        vertices.Store4(address, previousLow);
        vertices.Store4(address + 16, previousHigh);
#ifdef TF_GPU_DUAL_CAPTURE
        // Offline replay also needs the original packed GIF values. The native
        // vertex view uses only the first region; production omits this block.
        const uint rawAddress = count * 64 + address;
        const uint signedFlags = (flags & 32768) != 0 ? flags | 0xffff0000u : flags;
        previousRawLow = uint4(asuint(uvq), 0);
        previousRawHigh = uint4(uint3(xyz), signedFlags);
        vertices.Store4(rawAddress, previousRawLow);
        vertices.Store4(rawAddress + 16, previousRawHigh);
#endif
    }
}
