// A gradient fill for UI effect groups (UI::Effects::linear_gradient / radial_gradient / conic_gradient in UI/UiEffectLibrary.hpp).
// The widget's own drawing (any opaque background, including its rounded or bevelled corner shape) is only the *mask*: the gradient
// replaces its colour wherever the widget is drawn, so a gradient fills exactly the shape the element has.
//
// Colours are mixed with CSS colour interpolation (color_spaces.slang): each of the two segments between three stops has its own space
// (oklab, oklch, display-p3, hsl, ...) and, for a polar space, its own hue method, so a gradient can run through oklch for one stretch and
// plain sRGB for the next.
//
// Parameters (80 bytes): up to three colour stops.
//   c0, c1, c2 = stop colours in linear sRGB, straight alpha (out-of-gamut values are kept until the final write)
//   stops      = (position 0, position 1, position 2, kind)  with positions ascending in 0..1; kind 0 linear, 1 radial, 2 conic
//   geometry   = linear: (unused, unused, angle in radians, packed)  0 = left to right, increasing clockwise (y is down)
//                radial: (centre x, centre y in 0..1 of the element, radius as a fraction of the element's longer side, packed)
//                conic:  (centre x, centre y, start angle in radians, packed)
//   packed     = geometry.w reinterpreted as a uint: segment 0's space in bits 0-7 and hue method in 8-15, segment 1's in 16-23 and 24-31
// A gradient with fewer stops repeats its last colour in the spare slots.

import sturdy_common;
import color_spaces;

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

struct GradientParameters {
    UiEffectConstants frame;
    float4 c0;
    float4 c1;
    float4 c2;
    float4 stops;
    float4 geometry;
};

[[push_constant]] ConstantBuffer<GradientParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 mask = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 size = max(parameters.frame.groupSize, float2(1.0, 1.0));
    float2 local = input.uv * parameters.frame.contentSize - parameters.frame.groupOrigin; // pixels from the element's top-left
    float2 unit = local / size;                                                              // 0..1 across the element

    float t = 0.0;
    uint kind = uint(parameters.stops.w + 0.5);
    if (kind == 0u) {
        float2 direction = float2(cos(parameters.geometry.z), sin(parameters.geometry.z));
        float length_along = max(abs(dot(size, direction)), 1.0e-3); // CSS gradient line: the projection of the box on the direction
        t = dot(local - size * 0.5, direction) / length_along + 0.5;
    } else if (kind == 1u) {
        float2 offset = (unit - parameters.geometry.xy) * size;
        t = length(offset) / max(parameters.geometry.z * max(size.x, size.y), 1.0e-3);
    } else {
        float2 offset = (unit - parameters.geometry.xy) * size;
        float turn = (atan2(offset.y, offset.x) - parameters.geometry.z) / 6.28318530718;
        t = turn - floor(turn);
    }

    uint packed = asuint(parameters.geometry.w);
    uint space0 = packed & 0xFFu;
    uint hue0 = (packed >> 8u) & 0xFFu;
    uint space1 = (packed >> 16u) & 0xFFu;
    uint hue1 = (packed >> 24u) & 0xFFu;

    float4 color = parameters.c0;
    if (t >= parameters.stops.x) {
        float span1 = max(parameters.stops.y - parameters.stops.x, 1.0e-5);
        color = cssMix(parameters.c0, parameters.c1, saturate((t - parameters.stops.x) / span1), CSS_SRGB_LINEAR, space0, hue0, CSS_SRGB_LINEAR);
    }
    if (t >= parameters.stops.y) {
        float span2 = max(parameters.stops.z - parameters.stops.y, 1.0e-5);
        color = cssMix(parameters.c1, parameters.c2, saturate((t - parameters.stops.y) / span2), CSS_SRGB_LINEAR, space1, hue1, CSS_SRGB_LINEAR);
    }
    return float4(color.rgb * color.a, color.a) * mask.a;
}
