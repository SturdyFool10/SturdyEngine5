// Per-pixel saturation/value gradient plane for UI::color_picker() (UI/ColorPicker.hpp) — replaces
// what used to be an N×N grid of flat-colored Clay elements (one per gradient cell, capped at
// 32×32) with a single custom_element() draw call that computes the exact HSV->RGB color at every
// pixel analytically. Smooth at any size, and one draw call instead of up to 1024.
//
// UI::Context::custom_element()'s contract (CustomElement.hpp): the push-constant struct must
// start with UiElementConstants' five fields exactly, then whatever this shader needs.
// UiElementConstants' prefix is a clean 32 bytes (see its own doc comment), so the first trailing
// float4 field below starts immediately at byte 32 with no gap to account for. Two trailing
// float4s here: `params` (x = hue in degrees,
// y = opacity, z/w unused) and `cornerRadius` (topLeft, topRight, bottomLeft, bottomRight, matching
// Style.hpp's CornerRadius field order) — the surrounding element's own border can be rounded
// (UI::ColorPickerComposition::saturation_value.alter_decl), and a plain custom_element() draw is
// otherwise always sharp-cornered regardless of that (see roundedBoxSDF's own doc comment in
// sturdy_common.slang for why), so this shader masks itself to match.

import sturdy_common;

struct SvPlaneConstants {
    float2 position;
    float2 size;
    float2 viewportSize;
    float _reserved0;
    float _reserved1;
    float4 params;
    float4 cornerRadius;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<SvPlaneConstants> constants;

#else

[[push_constant]] ConstantBuffer<SvPlaneConstants> constants;

#endif
struct VertexOutput {
    float4 position : SV_Position;
    float2 localPixelPos : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.position = uiQuadClipPosition(vertexId, constants.position, constants.size, constants.viewportSize);
    output.localPixelPos = uiQuadCorner(vertexId) * constants.size;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float2 uv = clamp01(constants.size.x > 0.0 && constants.size.y > 0.0 ? input.localPixelPos / constants.size : float2(0.0, 0.0));
    float hue = constants.params.x;
    float opacity = clamp01(constants.params.y);
    // Matches ColorPicker's own convention: x = saturation (0 left -> 1 right), y = 1 - value
    // (0 top -> 1 bottom, i.e. value/brightness increases upward) — see the saturation_value
    // marker's own offset calculation in ColorPicker.hpp for the same mapping.
    float saturation = uv.x;
    float value = 1.0 - uv.y;

    float2 halfSize = constants.size * 0.5;
    float2 centerPos = input.localPixelPos - halfSize;
    float dist = roundedBoxSDF(centerPos, halfSize, constants.cornerRadius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float mask = clamp01(0.5 - dist / edgeSoftness);

    // hsvToRgb() produces sRGB-encoded channels (HSV is defined over the sRGB cube), but this
    // fragment writes into the same *Srgb render target every UI quad does — a target that encodes
    // linear -> sRGB on write. The generic quad path already accounts for that (ContextImpl.cpp's
    // clay_color_to_linear() linearizes every Clay color CPU-side before the GPU sees it), so
    // returning the sRGB value raw here double-encodes it, rendering the gradient visibly lighter
    // than the very color the picker then reports — the preview swatch (drawn as a Clay quad from
    // the same HSV) would not match the plane pixel under the cursor. Linearize to match.
    return float4(srgbToLinear(hsvToRgb(hue, saturation, value)), opacity * mask);
}
