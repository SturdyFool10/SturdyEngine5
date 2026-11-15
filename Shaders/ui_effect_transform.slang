// A 2D transform for UI effect groups (UI::Effects::transform in UI/UiEffectLibrary.hpp, CSS `transform` with `transform-origin`): the
// element and everything in it is drawn rotated, scaled, skewed-free and moved, without changing the layout it came from.
//
// Parameters (32 bytes):
//   inverse = (m00, m01, m10, m11) of the INVERSE linear part, row major: source offset = M * (output offset - translation)
//   place   = (translation x, translation y in pixels, origin x, origin y as a fraction of the element's box)
//
// For every pixel of the output the shader finds where it came from in the group's content and samples it there; anything that
// comes from outside the content is transparent. The content is premultiplied, so the result is too.

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

struct TransformParameters {
    UiEffectConstants frame;
    float4 inverse;
    float4 place;
};

[[push_constant]] ConstantBuffer<TransformParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 size = max(parameters.frame.contentSize, float2(1.0, 1.0));
    float2 pixel = input.uv * size;
    float2 origin = parameters.frame.groupOrigin + parameters.frame.groupSize * parameters.place.zw;
    float2 offset = pixel - origin - parameters.place.xy;
    float2 sourceOffset = float2(parameters.inverse.x * offset.x + parameters.inverse.y * offset.y,
                                 parameters.inverse.z * offset.x + parameters.inverse.w * offset.y);
    float2 sourceUv = (origin + sourceOffset) / size;
    if (any(sourceUv < float2(0.0, 0.0)) || any(sourceUv > float2(1.0, 1.0))) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return sourceTexture.SampleLevel(sourceSampler, sourceUv, 0.0);
}
