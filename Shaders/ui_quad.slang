// Instanced rect/border/image quad rendering for Sturdy.UI (UI/UiQuadPipeline.hpp).
//
// Vertex-pulled, same idiom as Shaders/text_sdf.slang: no vertex buffer, six vertices per instance
// built from SV_VertexID/SV_InstanceID. Every instance in one draw call shares the same texture (a
// 1x1 white pixel for plain rects, or a real image texture) — batching by (active scissor, and texture
// unless SFT_UI_BINDLESS puts them all in one array) happens on the CPU side (UI::UiRenderer::prepare()), keeping a whole UI frame a small,
// constant number of draw calls regardless of widget count.
//
// Rounding/borders use a signed-distance rounded-box test (Inigo Quilez's formula, adapted to this
// engine's per-corner radius ordering and top-left-origin/Y-down pixel space) rather than baking
// separate triangle geometry per corner, so a single quad shape handles every corner-radius/border
// combination Clay can produce.

import sturdy_common;

// The packed per-quad record the pipeline uploads: field order must byte-match UiQuadInstanceGpu in
// UI/UiQuad.hpp exactly. Radii, widths and colours are half floats (two per uint), which is plenty for
// pixel sizes and for 0..1 colour with HDR headroom, and takes the record from 112 to 72 bytes.
// `kindAndTexture` carries the quad kind in the low byte and, when textures are bound as an array
// (SFT_UI_BINDLESS), the texture's index in that array in the upper 24 bits.
struct UiQuadInstance {
    float2 position;
    float2 size;
    uint2 cornerRadiusHalf; // topLeft, topRight | bottomLeft, bottomRight
    uint2 borderWidthHalf;  // left, right | top, bottom
    uint2 fillColorHalf;    // r, g | b, a
    uint2 borderColorHalf;
    float2 uvMin;
    float2 uvMax;
    uint kindAndTexture; // kind: 0 = rect (solid and/or bordered), 1 = image (also SVG-sourced), 2 = image sampled nearest-neighbour
    uint reserved;       // keeps the record a multiple of 8 bytes, the stride a structured buffer of float2/uint2 members has
};

// Half to float by hand: f16tof32 makes the compiler declare the Float16/Int16 SPIR-V capabilities, which a device
// without those features does not allow, and nothing here needs more than the arithmetic below.
float halfToFloat(uint h) {
    uint exponent = (h >> 10u) & 0x1Fu;
    uint mantissa = h & 0x3FFu;
    float magnitude = exponent == 0u
        ? float(mantissa) * 5.9604644775390625e-8 // subnormal (and zero): mantissa * 2^-24
        : asfloat(((exponent + 112u) << 23u) | (mantissa << 13u)); // also carries infinity and NaN through
    return (h & 0x8000u) != 0u ? -magnitude : magnitude;
}

float4 unpackHalf4(uint2 packed) {
    return float4(halfToFloat(packed.x & 0xFFFFu), halfToFloat(packed.x >> 16u),
                  halfToFloat(packed.y & 0xFFFFu), halfToFloat(packed.y >> 16u));
}

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
#ifndef SFT_UI_BINDLESS
Texture2D<float4> quadTexture;
#endif
SamplerState quadSampler;
// Point-filtered twin of quadSampler for images drawn with UI::ImageSampling::Nearest (pixel art).
SamplerState quadSamplerNearest;
#ifdef SFT_UI_BINDLESS
// Every texture the frame draws, indexed by the quad's own texture index. Declared last because the
// variable-count binding of a descriptor set has to be its highest. Index 0 is the white pixel plain rects sample.
Texture2D<float4> quadTextures[];
#endif

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
    nointerpolation uint textureIndex : TEXCOORD6;
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
    output.cornerRadius = unpackHalf4(inst.cornerRadiusHalf);
    output.borderWidth = unpackHalf4(inst.borderWidthHalf);
    output.fillColor = unpackHalf4(inst.fillColorHalf);
    output.borderColor = unpackHalf4(inst.borderColorHalf);
    output.kind = float(inst.kindAndTexture & 0xFFu);
    output.textureIndex = inst.kindAndTexture >> 8u;
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
    // Both filters are fetched for the same reason, and the image kind picks one.
#ifdef SFT_UI_BINDLESS
    Texture2D<float4> quadTexture = quadTextures[NonUniformResourceIndex(input.textureIndex)];
#endif
    float4 sampledLinear = quadTexture.Sample(quadSampler, input.uv);
    float4 sampledNearest = quadTexture.Sample(quadSamplerNearest, input.uv);
    float4 sampled = input.kind > 1.5 ? sampledNearest : sampledLinear;

    float4 color = input.fillColor;
    if (input.kind > 0.5) {
        // Image (UI::Context::image(), also every UI::Context::svg()-drawn icon — an SVG-sourced
        // texture is rasterized to plain RGBA8 ahead of time by Engine::UiSvgCache, so it's
        // indistinguishable from any other image by the time it reaches this shader).
        // Clay_ImageRenderData's default tint is (0,0,0,0), meaning "untinted" (see its own doc
        // comment) — only multiply-tint when the caller actually set a tint color. The tint's alpha scales the
        // image's opacity (fade a thumbnail in, dim a disabled icon).
        float4 tint = input.fillColor.a > 0.0 ? input.fillColor : float4(1.0, 1.0, 1.0, 1.0);
        color = float4(sampled.rgb * tint.rgb, sampled.a * tint.a);
    } else if (dot(input.borderWidth, input.borderWidth) > 0.0) {
        float2 innerHalfSize = input.halfSize - float2(
            (input.borderWidth.x + input.borderWidth.y) * 0.5,
            (input.borderWidth.z + input.borderWidth.w) * 0.5);
        float2 innerCenter = centerPos - float2(
            (input.borderWidth.x - input.borderWidth.y) * 0.5,
            (input.borderWidth.z - input.borderWidth.w) * 0.5);
        float maxBorder = max(max(input.borderWidth.x, input.borderWidth.y),
                              max(input.borderWidth.z, input.borderWidth.w));
        float4 innerRadius = max(input.cornerRadius - maxBorder, 0.0);
        float innerDist = roundedBoxSDF(innerCenter, max(innerHalfSize, 0.0), innerRadius);
        float borderWeight = clamp01(0.5 + innerDist / edgeSoftness);
        color = lerp(input.fillColor, input.borderColor, borderWeight);
    }

    return float4(color.rgb, color.a * outerAlpha);
}
