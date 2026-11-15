// One axis of a Gaussian blur, for UI effect groups (UI/UiEffect.hpp). A blur is two passes of this shader, horizontal then vertical,
// each reading the previous result: build the effect with a pass whose parameters say (1, 0) and a second that reads its output and
// says (0, 1). Running the passes at a smaller output `scale` (0.5, 0.25) widens the blur cheaply; sizes are in the source's texels.
//
// Parameters (16 bytes): `blur.xy` is the direction of one tap in source texels (1,0 or 0,1), `blur.z` the Gaussian sigma in source
// texels, `blur.w` how far apart consecutive taps are (1 = every texel, 2 = every other, ...; larger reaches further for the same cost).
// Thirteen taps, so a sigma above about 2 * tap spacing starts to show banding; stack passes instead.
//
// The content is premultiplied, so averaging it is already correct: no alpha weighting is needed.

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

struct BlurParameters {
    UiEffectConstants frame;
    float4 blur;
};

[[push_constant]] ConstantBuffer<BlurParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    uint width;
    uint height;
    sourceTexture.GetDimensions(width, height);
    float2 texel = 1.0 / float2(max(width, 1u), max(height, 1u));
    float2 step = parameters.blur.xy * parameters.blur.w * texel;
    float sigma = max(parameters.blur.z, 0.01);

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    [unroll]
    for (int i = -6; i <= 6; ++i) {
        float offset = float(i) * parameters.blur.w;
        float weight = exp(-(offset * offset) / (2.0 * sigma * sigma));
        sum += sourceTexture.SampleLevel(sourceSampler, input.uv + step * float(i), 0.0) * weight;
        weightSum += weight;
    }
    return sum / weightSum;
}
