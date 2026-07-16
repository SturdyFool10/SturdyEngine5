// Instanced SDF/MSDF/Color glyph-quad rendering (Renderer/TextInstance.cppm's TextPipeline).
//
// Vertex-pulled: no vertex buffer, six vertices per instance (two triangles) built entirely from
// SV_VertexID/SV_InstanceID, matching the fullscreen-triangle idiom in sturdy_common.slang and
// Shaders/fullscreen_tonemap.slang. Each instance is one glyph quad; every instance in one draw
// call samples the same atlas tile texture — batching by (format, tile) happens on the CPU side
// (TextPipeline::prepare()), which is what makes a whole run of text a small, constant number of
// draw calls regardless of glyph count.
//
// Antialiasing is coverage-based, not subpixel: MSDF's RGB channels are distance channels,
// collapsed through median3() before any color math (see sturdy_common.slang) — never sampled as
// per-channel subpixel color — so there is no color fringing. Output alpha is the fraction of the
// pixel the glyph's ink covers, following msdfgen's own reference reconstruction shader. Color
// glyphs (emoji — Text::RasterFormat::Color, Text/ColorGlyph.cppm) skip the distance math
// entirely: the atlas cell already holds the final flat-shaded RGBA, sampled straight through and
// only alpha-modulated by the instance color.

import sturdy_common;

// Field order must byte-match Renderer/TextInstance.hpp's GlyphInstance exactly (see the comment
// there) - two vec2 pairs, then the vec4, then four floats, with no gaps under std430 either way.
struct GlyphInstance {
    float2 position;
    float2 size;
    float2 uvMin;
    float2 uvMax;
    float4 color;
    // Radians, counter-clockwise, rotates the quad's size axes around `position` — text-on-a-spline
    // (Renderer/Spline.cppm) and ordinary rotated 2D labels both feed this.
    float rotation;
    // 0 = SDF, 1 = MSDF, 2 = Color — see Renderer::format_kind_value (Renderer/TextInstance.cppm).
    float formatKind;
    // Encoded distance-band width in atlas texels. Converted to screen pixels from UV derivatives
    // in fragmentMain, so non-uniform transforms remain correctly antialiased.
    float distancePixelRange;
    // Embolden bias, in screen pixels, added to the coverage threshold — see
    // Renderer::resolved_stem_darkening_px (Renderer/TextInstance.hpp).
    float stemDarkeningPx;
};

struct TextViewConstants {
    float2 viewportSize;
    // Explicit base into `instances` for the current atlas-image batch. SV_InstanceID's treatment
    // of API draw-call base-instance values is not portable across shader-language backends.
    uint instanceIndexBase;
    uint padding;
};

[[push_constant]] ConstantBuffer<TextViewConstants> viewConstants;
StructuredBuffer<GlyphInstance> instances;
Texture2D<float4> atlasTexture;
SamplerState atlasSampler;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float formatKind : TEXCOORD1;
    float distancePixelRange : TEXCOORD2;
    float stemDarkeningPx : TEXCOORD3;
};

static const float2 kQuadCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
    float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    GlyphInstance inst = instances[viewConstants.instanceIndexBase + instanceId];
    float2 corner = kQuadCorners[vertexId % 6];

    // Build the quad's size axes pre-rotated by `inst.rotation` instead of an axis-aligned
    // `corner * size` — at rotation 0 this is identical to the unrotated form (right=(size.x,0),
    // up=(0,size.y)), so ordinary flat text is unaffected.
    float cosR = cos(inst.rotation);
    float sinR = sin(inst.rotation);
    float2 right = float2(cosR, sinR) * inst.size.x;
    float2 up = float2(-sinR, cosR) * inst.size.y;
    float2 pixelPos = inst.position + corner.x * right + corner.y * up;
    // Pixel space -> clip space: same direct (unflipped) mapping as fullscreen_tonemap.slang's
    // fullscreen triangle — Vulkan NDC Y and this engine's pixel-space Y both point down.
    float2 ndc = (pixelPos * safeRcp(viewConstants.viewportSize)) * 2.0 - 1.0;

    VertexOutput output;
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = lerp(inst.uvMin, inst.uvMax, corner);
    output.color = inst.color;
    output.formatKind = inst.formatKind;
    output.distancePixelRange = inst.distancePixelRange;
    output.stemDarkeningPx = inst.stemDarkeningPx;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    if (input.formatKind > 1.5) {
        // Color (emoji): the atlas cell is already the final flat-shaded RGBA — no distance field,
        // no median, just sample and modulate alpha by the instance color (never tint the RGB,
        // emoji carry their own color).
        float4 sampled = atlasTexture.Sample(atlasSampler, input.uv);
        return float4(sampled.rgb, sampled.a * input.color.a);
    }

    float signedDistance;
    if (input.formatKind > 0.5) {
        float3 sampled = atlasTexture.Sample(atlasSampler, input.uv).rgb;
        signedDistance = median3(sampled);
    } else {
        signedDistance = atlasTexture.Sample(atlasSampler, input.uv).r;
    }

    // 0.5 sits exactly on the glyph's true edge (Text::rasterize_glyph's distance mapping);
    // Convert the atlas-texel distance range to screen pixels using the local UV derivatives.
    // This is msdfgen's reference reconstruction and remains correct for non-uniformly scaled or
    // rotated quads, where one CPU-side pixels-per-texel estimate cannot represent both axes.
    // stemDarkeningPx shifts that threshold outward by a few tenths of a pixel at small sizes
    // (Renderer::resolved_stem_darkening_px). Scalable unhinted text uses it to retain small-size
    // weight; fixed-size grid-fitted callers pass zero so compensation is never doubled.
    uint atlasWidth;
    uint atlasHeight;
    atlasTexture.GetDimensions(atlasWidth, atlasHeight);
    float2 unitRange = input.distancePixelRange / float2(atlasWidth, atlasHeight);
    float2 screenTexelSpan = 1.0 / max(fwidth(input.uv), float2(SFT_EPSILON));
    float screenPxRange = max(0.5, 0.5 * dot(unitRange, screenTexelSpan));
    float screenPxDistance = screenPxRange * (signedDistance - 0.5);
    float coverage = clamp01(screenPxDistance + 0.5 + input.stemDarkeningPx);
    return float4(input.color.rgb, input.color.a * coverage);
}
