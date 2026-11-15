// Captures the part of the render target that lies behind an effect group (what UiEffectInputKind::Backdrop reads).
//
// Parameters (16 bytes): area = (region origin x, y, target width, height), all in surface pixels. The output is the region's pixels
// copied from the target, opaque.

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

struct BackdropParameters {
    UiEffectConstants frame;
    float4 area;
};

[[push_constant]] ConstantBuffer<BackdropParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 pixel = parameters.area.xy + input.uv * parameters.frame.contentSize;
    float4 color = sourceTexture.SampleLevel(sourceSampler, pixel / max(parameters.area.zw, float2(1.0, 1.0)), 0.0);
    return float4(color.rgb, 1.0);
}
