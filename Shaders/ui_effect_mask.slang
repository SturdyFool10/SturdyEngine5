// CSS `mask-image` for UI effect groups (UI::Effects::mask_linear / mask_image in UI/UiEffectLibrary.hpp): the element's pixels are
// multiplied by a mask, so it can fade out along a gradient or take the shape of an image.
//
// Inputs: `sourceTexture` is the group's content; `extraTexture0` is the mask image (mode 1 only), sampled across the element's box.
// Parameters (32 bytes):
//   header = (mode, invert, use luminance, unused)       mode 0: linear alpha ramp, mode 1: image
//   ramp   = (angle in radians, alpha at start, alpha at end, unused) + positions (start, end) in `place`
//   place  = (ramp start 0..1, ramp end 0..1, unused, unused)
//
// A ramp runs along `angle` (0 = left to right, clockwise) across the box. The content is premultiplied, so scaling it keeps it so.

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

struct MaskParameters {
    UiEffectConstants frame;
    float4 header;
    float4 ramp;
    float4 place;
};

[[push_constant]] ConstantBuffer<MaskParameters> parameters;

Texture2D<float4> sourceTexture;
Texture2D<float4> extraTexture0;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 pixel = input.uv * parameters.frame.contentSize;
    float2 boxSize = max(parameters.frame.groupSize, float2(1.0, 1.0));
    float2 boxUv = (pixel - parameters.frame.groupOrigin) / boxSize;
    float mask = 1.0;
    if (parameters.header.x < 0.5) {
        float2 direction = float2(cos(parameters.ramp.x), sin(parameters.ramp.x));
        // Project the box corners onto the direction so 0..1 spans the whole box, as CSS gradient lines do.
        float2 centered = (boxUv - float2(0.5, 0.5)) * boxSize;
        float extent = abs(direction.x) * boxSize.x + abs(direction.y) * boxSize.y;
        float t = dot(centered, direction) / max(extent, 1.0e-4) + 0.5;
        float span = max(parameters.place.y - parameters.place.x, 1.0e-4);
        float f = saturate((t - parameters.place.x) / span);
        mask = lerp(parameters.ramp.y, parameters.ramp.z, f);
    } else {
        float4 texel = extraTexture0.SampleLevel(sourceSampler, boxUv, 0.0);
        bool outside = any(boxUv < float2(0.0, 0.0)) || any(boxUv > float2(1.0, 1.0));
        float luminance = dot(texel.rgb, float3(0.2126, 0.7152, 0.0722)) * texel.a;
        mask = parameters.header.z > 0.5 ? luminance : texel.a;
        if (outside) mask = 0.0;
    }
    if (parameters.header.y > 0.5) mask = 1.0 - mask;
    return content * saturate(mask);
}
