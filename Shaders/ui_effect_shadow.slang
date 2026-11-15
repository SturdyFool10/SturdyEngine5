// The last pass of a UI drop shadow or glow (UI::Effects::drop_shadow / glow in UI/UiEffectLibrary.hpp): lays a blurred copy of the
// widget's shape, coloured and offset, *under* the widget itself.
//
// Inputs: `sourceTexture` is the group's content; `extraTexture0` is that content blurred (any resolution; it is sampled by uv).
// Parameters (32 bytes):
//   color = (r, g, b, a) of the shadow in linear light, straight alpha
//   shape = (offset x, offset y in surface pixels, strength, unused)  -- strength scales the blurred alpha (above 1 makes a glow dense)
//
// The content is premultiplied, so source-over is `content + shadow * (1 - content.a)`.

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

struct ShadowParameters {
    UiEffectConstants frame;
    float4 color;
    float4 shape;
};

[[push_constant]] ConstantBuffer<ShadowParameters> parameters;

Texture2D<float4> sourceTexture;
Texture2D<float4> extraTexture0;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 shadowUv = input.uv - parameters.shape.xy / max(parameters.frame.contentSize, float2(1.0, 1.0));
    float coverage = saturate(extraTexture0.SampleLevel(sourceSampler, shadowUv, 0.0).a * parameters.shape.z);
    float shadowAlpha = parameters.color.a * coverage;
    float4 shadow = float4(parameters.color.rgb * shadowAlpha, shadowAlpha);
    return content + shadow * (1.0 - content.a);
}
