// A one-pass UI effect: inverts and/or tints a widget as a whole. It is both a ready-made colour effect and the smallest example of
// the UI effect shader contract (UI/UiEffect.hpp):
//
//   * `vertexMain` is the engine's fullscreen triangle; `fragmentMain` reads the group's content (premultiplied RGBA).
//   * Inputs are `Texture2D sourceTexture` (the first input) and `Texture2D extraTexture<N>`, all through `SamplerState sourceSampler`.
//   * One `[[push_constant]]` block that starts with UiEffectConstants (sturdy_common.slang) and continues with the pass's parameters.
//
// Parameters (32 bytes): `tint` multiplies the result's colour and alpha (premultiplied, so tint.rgb scales the colour and tint.a the
// whole widget's opacity); `invert.x` is how far to invert the widget's colour, 0 (unchanged) to 1 (fully inverted, alpha kept).

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = fullscreenTriangleUv(vertexId);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

struct TintParameters {
    UiEffectConstants frame;
    float4 tint;
    float4 invert; // x = amount; yzw unused (keeps the block a whole number of 16-byte registers)
};

[[push_constant]] ConstantBuffer<TintParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    // Premultiplied colour: the inverse of a colour c at coverage a is (1 - c) * a = a - premultiplied(c).
    float3 inverted = float3(content.a, content.a, content.a) - content.rgb;
    float3 rgb = lerp(content.rgb, inverted, saturate(parameters.invert.x));
    return float4(rgb * parameters.tint.rgb, content.a) * parameters.tint.a;
}
