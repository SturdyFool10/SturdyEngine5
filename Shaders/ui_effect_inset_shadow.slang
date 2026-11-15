// The last pass of an inset shadow (UI::Effects::inset_shadow in UI/UiEffectLibrary.hpp, CSS `box-shadow: inset`): a shadow that falls
// *inside* the element's shape, darkest along its edges, drawn over the content and clipped to it.
//
// Inputs: `sourceTexture` is the group's content; `extraTexture0` is that content blurred (any resolution, sampled by uv).
// Parameters (32 bytes):
//   color = (r, g, b, a) of the shadow in linear light, straight alpha
//   shape = (offset x, offset y in surface pixels, strength, unused)
//
// The blurred alpha is how much of the neighbourhood is covered, so 1 minus it is how close the edge is: that is the shadow's density.
// Moving the sample by `offset` slides the shadow towards one side, as an inset shadow with an offset does.

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

struct InsetShadowParameters {
    UiEffectConstants frame;
    float4 color;
    float4 shape;
};

[[push_constant]] ConstantBuffer<InsetShadowParameters> parameters;

Texture2D<float4> sourceTexture;
Texture2D<float4> extraTexture0;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 sampleUv = input.uv - parameters.shape.xy / max(parameters.frame.contentSize, float2(1.0, 1.0));
    float covered = extraTexture0.SampleLevel(sourceSampler, sampleUv, 0.0).a;
    float density = saturate((1.0 - covered) * parameters.shape.z) * parameters.color.a;
    float shadowAlpha = density * content.a; // only where the element is
    float4 shadow = float4(parameters.color.rgb * shadowAlpha, shadowAlpha);
    return shadow + content * (1.0 - shadowAlpha);
}
