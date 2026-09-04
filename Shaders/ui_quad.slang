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
    float kind; // 0 = rect (solid and/or bordered), 1 = image (also SVG-sourced — see UiQuad.hpp)
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

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<UiViewConstants> viewConstants;

#else

[[push_constant]] ConstantBuffer<UiViewConstants> viewConstants;

#endif
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

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    UiQuadInstance inst = instances[viewConstants.instanceIndexBase + instanceId];
    float2 corner = uiQuadCorner(vertexId);
    float2 localPixelPos = corner * inst.size;
    float2 pixelPos = inst.position + localPixelPos;
    float2 pixelUv = pixelPos * safeRcp(viewConstants.viewportSize);

    VertexOutput output;
    output.position = float4(uvToNdc(pixelUv), 0.0, 1.0);
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

// roundedBoxSDF() now lives in sturdy_common.slang (shared with UI::Context::custom_element()
// shaders that need the same rounded-corner mask — see its own doc comment there).

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 centerPos = input.localPixelPos - input.halfSize;
    float outerDist = roundedBoxSDF(centerPos, input.halfSize, input.cornerRadius);
    float edgeSoftness = max(fwidth(outerDist), SFT_EPSILON);
    float outerAlpha = clamp01(0.5 - outerDist / edgeSoftness);

    // Sampled unconditionally, before the branch that actually wants it. An implicit-LOD sample is
    // only defined when every invocation in the quad performs it, and `kind` varies per element, so
    // sampling inside the branch is a uniformity violation: SPIR-V accepts it, WGSL rejects the
    // shader outright. A quad's texture binding is always a valid texture -- UiRenderer binds a
    // white fallback view for every untextured fill -- so the extra fetch on the non-image path is
    // well-defined and its result simply goes unused.
    float4 sampled = quadTexture.Sample(quadSampler, input.uv);

    float4 color = input.fillColor;
    if (input.kind > 0.5) {
        // Image (UI::Context::image(), also every UI::Context::svg()-drawn icon — an SVG-sourced
        // texture is rasterized to plain RGBA8 ahead of time by Engine::UiSvgCache, so it's
        // indistinguishable from any other image by the time it reaches this shader).
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
