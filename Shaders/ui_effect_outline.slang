// An outline around an element's shape (UI::Effects::outline in UI/UiEffectLibrary.hpp): a ring of `color`, `width` pixels wide, starting
// `offset` pixels outside the shape's edge (CSS `outline` with `outline-offset`). It follows whatever shape the element has (corner shapes,
// an image's alpha, text) because it is made from the content's alpha: the ring is where the shape grown by offset + width is covered
// but the shape grown by offset is not. Drawn under the content.
//
// Parameters (32 bytes):
//   color = (r, g, b, a) of the ring, linear, straight alpha
//   ring  = (width, offset, unused, unused) in surface pixels
//
// Growing the shape is a maximum of the content's alpha over 16 directions at three radii, so a ring wider than about 16 px starts to show
// the 16-gon it is made from.

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

struct OutlineParameters {
    UiEffectConstants frame;
    float4 color;
    float4 ring;
};

[[push_constant]] ConstantBuffer<OutlineParameters> parameters;

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

// The shape's alpha grown by `radius` pixels: the largest alpha within that distance.
float grownAlpha(float2 uv, float radius, float2 texel) {
    float best = sourceTexture.SampleLevel(sourceSampler, uv, 0.0).a;
    if (radius <= 0.0) {
        return best;
    }
    [unroll]
    for (int ring = 1; ring <= 3; ++ring) {
        float r = radius * float(ring) / 3.0;
        [unroll]
        for (int i = 0; i < 16; ++i) {
            float angle = float(i) * 0.39269908; // 2 pi / 16
            float2 offset = float2(cos(angle), sin(angle)) * r * texel;
            best = max(best, sourceTexture.SampleLevel(sourceSampler, uv + offset, 0.0).a);
        }
    }
    return best;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 content = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    float2 texel = 1.0 / max(parameters.frame.contentSize, float2(1.0, 1.0));
    float width = max(parameters.ring.x, 0.0);
    float offset = max(parameters.ring.y, 0.0);
    float outer = grownAlpha(input.uv, offset + width, texel);
    float inner = offset > 0.0 ? grownAlpha(input.uv, offset, texel) : content.a;
    float ringAlpha = saturate(outer - inner) * parameters.color.a;
    float4 ring = float4(parameters.color.rgb * ringAlpha, ringAlpha);
    return content + ring * (1.0 - content.a);
}
