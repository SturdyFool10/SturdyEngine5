// Frosted glass (UI::Effects::frosted_glass in UI/UiEffectLibrary.hpp, CSS `backdrop-filter: blur()`): the element's shape is filled with
// a blurred copy of what is behind it, tinted, with the element's own content drawn on top.
//
// Inputs: `sourceTexture` is the backdrop (UiEffectInputKind::Backdrop); `extraTexture0` is the group's content (premultiplied RGBA).
// Parameters (32 bytes):
//   tint = (r, g, b, strength): the backdrop is mixed toward this colour by `strength`
//   blur = (radius in pixels, saturation, unused, unused)
//
// The blur is a golden-angle disc of taps whose radius is `blur.x`; the element's coverage is the mask, so corner shapes and partial fills
// carry through.

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

struct FrostedParameters {
    UiEffectConstants frame;
    float4 tint;
    float4 blur;
};

[[push_constant]] ConstantBuffer<FrostedParameters> parameters;

Texture2D<float4> sourceTexture;
Texture2D<float4> extraTexture0;
SamplerState sourceSampler;

static const int kTaps = 48;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = extraTexture0.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 texel = 1.0 / max(parameters.frame.contentSize, float2(1.0, 1.0));
    float radius = max(parameters.blur.x, 0.0);
    float3 sum = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0).rgb;
    [unroll]
    for (int i = 1; i < kTaps; ++i) {
        float r = sqrt(float(i) / float(kTaps)) * radius;
        float a = float(i) * 2.39996323;
        float2 offset = float2(cos(a), sin(a)) * r * texel;
        sum += sourceTexture.SampleLevel(sourceSampler, clamp(input.uv + offset, texel * 0.5, 1.0 - texel * 0.5), 0.0).rgb;
    }
    float3 blurred = sum / float(kTaps);
    float luma = dot(blurred, float3(0.2126, 0.7152, 0.0722));
    blurred = lerp(float3(luma, luma, luma), blurred, parameters.blur.y);
    blurred = lerp(blurred, parameters.tint.rgb, saturate(parameters.tint.w));
    // Any coverage at all means "inside the shape" (so a faint fill still gets full glass); the edge keeps its antialiasing.
    float mask = saturate(content.a * 32.0);
    // Glass under the element's shape, then the element's own drawing over it.
    float3 glass = blurred * mask;
    return float4(glass * (1.0 - content.a) + content.rgb, max(mask, content.a));
}
