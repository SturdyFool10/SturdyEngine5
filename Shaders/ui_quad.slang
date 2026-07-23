// Instanced rect/border/image quad rendering for Sturdy.UI (UI/UiQuadPipeline.hpp).
//
// Vertex-pulled, same idiom as Shaders/text_sdf.slang: no vertex buffer, six vertices per instance
// built from SV_VertexID/SV_InstanceID. Every instance in one draw call shares the same texture (a
// 1x1 white pixel for plain rects, or a real image texture) — batching by (kind, texture, active
// scissor) happens on the CPU side (UI::UiRenderer::prepare()), keeping a whole UI frame a small,
// constant number of draw calls regardless of widget count.
//
// Rounding/borders use a signed-distance rounded-box test (Inigo Quilez's formula, adapted to this
// engine's per-corner radius ordering and top-left-origin/Y-down pixel space) rather than baking
// separate triangle geometry per corner, so a single quad shape handles every corner-radius/border
// combination Clay can produce.

import sturdy_common;

// Field order must byte-match UI/UiQuad.hpp's UiQuadInstance exactly (see the comment there).
struct UiQuadInstance {
    float2 position;
    float2 size;
    float4 cornerRadius; // topLeft, topRight, bottomLeft, bottomRight
    float4 borderWidth;  // left, right, top, bottom
    float4 fillColor;
    float4 borderColor;
    float2 uvMin;
    float2 uvMax;
    float kind; // 0 = rect (solid and/or bordered), 1 = image
    float _pad0;
    float _pad1;
    float _pad2;
};

struct UiViewConstants {
    float2 viewportSize;
    // Explicit base into `instances` for the current batch — see text_sdf.slang's identical
    // `instanceIndexBase` field for why this isn't left to SV_InstanceID/first_instance alone.
    uint instanceIndexBase;
    uint padding;
};

[[push_constant]] ConstantBuffer<UiViewConstants> viewConstants;
StructuredBuffer<UiQuadInstance> instances;
Texture2D<float4> quadTexture;
SamplerState quadSampler;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 localPixelPos : TEXCOORD1; // pixel offset from the quad's top-left, for the SDF test
    float2 halfSize : TEXCOORD2;
    float4 cornerRadius : TEXCOORD3;
    float4 borderWidth : TEXCOORD4;
    float4 fillColor : COLOR0;
    float4 borderColor : COLOR1;
    float kind : TEXCOORD5;
};

static const float2 kQuadCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
    float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    UiQuadInstance inst = instances[viewConstants.instanceIndexBase + instanceId];
    float2 corner = kQuadCorners[vertexId % 6];
    float2 localPixelPos = corner * inst.size;
    float2 pixelPos = inst.position + localPixelPos;
    // Pixel space -> clip space: same direct (unflipped) mapping as text_sdf.slang / the
    // fullscreen-triangle idiom in sturdy_common.slang.
    float2 ndc = (pixelPos * safeRcp(viewConstants.viewportSize)) * 2.0 - 1.0;

    VertexOutput output;
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = lerp(inst.uvMin, inst.uvMax, corner);
    output.localPixelPos = localPixelPos;
    output.halfSize = inst.size * 0.5;
    output.cornerRadius = inst.cornerRadius;
    output.borderWidth = inst.borderWidth;
    output.fillColor = inst.fillColor;
    output.borderColor = inst.borderColor;
    output.kind = inst.kind;
    return output;
}

// Signed distance to a per-corner rounded box, `centerPos` relative to the box center (Y down,
// matching this engine's pixel-space convention) and `halfSize` the box's half-extent. Negative
// inside the shape, positive outside, zero on the boundary — msdfgen-style reference reconstruction
// wants exactly this convention for antialiasing via fwidth() below.
float roundedBoxSDF(float2 centerPos, float2 halfSize, float4 cornerRadius) {
    bool left = centerPos.x <= 0.0;
    bool top = centerPos.y <= 0.0;
    float radius = left ? (top ? cornerRadius.x : cornerRadius.z) : (top ? cornerRadius.y : cornerRadius.w);
    radius = min(radius, min(halfSize.x, halfSize.y));
    float2 q = abs(centerPos) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 centerPos = input.localPixelPos - input.halfSize;
    float outerDist = roundedBoxSDF(centerPos, input.halfSize, input.cornerRadius);
    float edgeSoftness = max(fwidth(outerDist), SFT_EPSILON);
    float outerAlpha = clamp01(0.5 - outerDist / edgeSoftness);

    float4 color = input.fillColor;
    if (input.kind > 0.5) {
        float4 sampled = quadTexture.Sample(quadSampler, input.uv);
        // Clay_ImageRenderData's default tint is (0,0,0,0), meaning "untinted" (see its own doc
        // comment) — only multiply-tint when the caller actually set a tint color.
        float3 tinted = input.fillColor.a > 0.0 ? sampled.rgb * input.fillColor.rgb : sampled.rgb;
        color = float4(tinted, sampled.a);
    } else if (dot(input.borderWidth, input.borderWidth) > 0.0) {
        float2 innerHalfSize = input.halfSize - float2(
            (input.borderWidth.x + input.borderWidth.y) * 0.5,
            (input.borderWidth.z + input.borderWidth.w) * 0.5);
        float2 innerCenter = centerPos - float2(
            (input.borderWidth.y - input.borderWidth.x) * 0.5,
            (input.borderWidth.w - input.borderWidth.z) * 0.5);
        float maxBorder = max(max(input.borderWidth.x, input.borderWidth.y),
                              max(input.borderWidth.z, input.borderWidth.w));
        float4 innerRadius = max(input.cornerRadius - maxBorder, 0.0);
        float innerDist = roundedBoxSDF(innerCenter, max(innerHalfSize, 0.0), innerRadius);
        float borderWeight = clamp01(0.5 + innerDist / edgeSoftness);
        color = lerp(input.fillColor, input.borderColor, borderWeight);
    }

    return float4(color.rgb, color.a * outerAlpha);
}
