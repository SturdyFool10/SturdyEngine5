// A one-pass colour adjustment for UI effect groups (UI::Effects::color_adjust in UI/UiEffectLibrary.hpp): brightness, contrast,
// saturation, grayscale, sepia, hue rotation, invert and opacity, applied to a widget as a whole. Works in linear light on the
// un-premultiplied colour and premultiplies again, so transparent edges stay clean.
//
// Parameters (32 bytes):
//   adjust = (brightness, contrast, saturation, grayscale)  -- 1, 1, 1, 0 leaves the colour unchanged
//   extra  = (sepia, invert, hue rotation in radians, opacity) -- 0, 0, 0, 1 leaves it unchanged

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

struct ColorParameters {
    UiEffectConstants frame;
    float4 adjust;
    float4 extra;
};

[[push_constant]] ConstantBuffer<ColorParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

float lumaOf(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float3 rgb = content.a > 1.0e-5 ? content.rgb / content.a : float3(0.0, 0.0, 0.0);

    rgb *= parameters.adjust.x;
    rgb = (rgb - 0.5) * parameters.adjust.y + 0.5;
    rgb = lerp(float3(lumaOf(rgb), lumaOf(rgb), lumaOf(rgb)), rgb, parameters.adjust.z);
    float grey = lumaOf(rgb);
    rgb = lerp(rgb, float3(grey, grey, grey), saturate(parameters.adjust.w));

    float3 sepia = float3(dot(rgb, float3(0.393, 0.769, 0.189)), dot(rgb, float3(0.349, 0.686, 0.168)),
                          dot(rgb, float3(0.272, 0.534, 0.131)));
    rgb = lerp(rgb, sepia, saturate(parameters.extra.x));

    // Hue rotation: rotate around the grey axis (1,1,1)/sqrt(3) with Rodrigues' formula.
    float angle = parameters.extra.z;
    float3 axis = float3(0.57735027, 0.57735027, 0.57735027);
    rgb = rgb * cos(angle) + cross(axis, rgb) * sin(angle) + axis * dot(axis, rgb) * (1.0 - cos(angle));

    rgb = lerp(rgb, float3(1.0, 1.0, 1.0) - rgb, saturate(parameters.extra.y));
    rgb = max(rgb, 0.0);
    return float4(rgb * content.a, content.a) * parameters.extra.w;
}
