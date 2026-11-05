// SPDX-License-Identifier: GPL-3.0-or-later
cbuffer SceneDraw : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
    float4 baseColor;
};
Texture2D<float4> albedo : register(t0);
SamplerState linearRepeat : register(s0);
struct Input {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct Varying {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
Varying vsMain(Input input)
{
    Varying output;
    output.position = mul(mul(float4(input.position, 1), model), viewProjection);
    output.uv = input.uv;
    output.color = input.color * baseColor;
    return output;
}
float4 psMain(Varying input) : SV_Target0
{
    return albedo.Sample(linearRepeat, input.uv) * input.color;
}
