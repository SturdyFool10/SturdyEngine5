// One step of a mip chain for UI effect targets (UI::UiEffectTarget::mip_levels, UI::UiEffectContent::mip_levels): writes level N from the
// single-level view of level N-1. The renderer runs it once per level, so a chain costs one cheap fullscreen pass per level and a later
// pass can read the whole chain at a few taps.
//
// Parameters (16 bytes): mode.x 0 = box (the average of each 2x2 block: one bilinear tap at its centre), 1 = tent (a 3x3 tent over the
// level above, so a blur read from a high level is smooth rather than blocky); mode.yz = the source level's texel size (1 / width,
// 1 / height). The content is premultiplied, so averaging it needs no alpha weighting.

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

struct DownsampleParameters {
    UiEffectConstants frame;
    float4 mode;
};

[[push_constant]] ConstantBuffer<DownsampleParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    if (parameters.mode.x < 0.5) {
        return sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    }
    float2 texel = parameters.mode.yz;
    float4 sum = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0) * 4.0;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(-texel.x, 0.0), 0.0) * 2.0;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(texel.x, 0.0), 0.0) * 2.0;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(0.0, -texel.y), 0.0) * 2.0;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(0.0, texel.y), 0.0) * 2.0;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(-texel.x, -texel.y), 0.0);
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(texel.x, -texel.y), 0.0);
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(-texel.x, texel.y), 0.0);
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(texel.x, texel.y), 0.0);
    return sum / 16.0;
}
