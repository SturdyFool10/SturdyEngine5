// A wide blur in one pass, for UI effects that ask for mips (UI::Effects::fast_blur in UI/UiEffectLibrary.hpp). The input has a mip chain
// (UI::UiEffectContent::mip_levels or UI::UiEffectTarget::mip_levels), every level of which is already an average of the one above, so
// reading a high level *is* a blur, at a cost that does not grow with its radius. Five taps (the centre and four diagonal neighbours at half
// a texel of that level) smooth the blockiness a single trilinear read of a coarse level would show.
//
// Parameters (16 bytes): blur.x = the level to read (fractional: the sampler blends the two nearest levels); blur.y = how far apart the
// four outer taps are, in texels of that level (0.5 is a good default).
//
// The input's sampler must allow the chain (UiEffectSampler::max_lod at its default 1000 does) and mip_filter Linear for fractional levels.

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

struct MipBlurParameters {
    UiEffectConstants frame;
    float4 blur;
};

[[push_constant]] ConstantBuffer<MipBlurParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float lod = parameters.blur.x;
    uint width;
    uint height;
    sourceTexture.GetDimensions(width, height); // level 0 of whatever the input is: the content, or a scaled pass output
    float2 texel = exp2(lod) / float2(max(width, 1u), max(height, 1u)); // one texel of that level, in uv
    float2 spread = texel * parameters.blur.y;
    float4 sum = sourceTexture.SampleLevel(sourceSampler, input.uv, lod) * 0.4;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(-spread.x, -spread.y), lod) * 0.15;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(spread.x, -spread.y), lod) * 0.15;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(-spread.x, spread.y), lod) * 0.15;
    sum += sourceTexture.SampleLevel(sourceSampler, input.uv + float2(spread.x, spread.y), lod) * 0.15;
    return sum;
}
