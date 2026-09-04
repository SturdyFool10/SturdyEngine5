// Preview swatch for UI::color_picker() (UI/ColorPicker.hpp): the picked color composited at its
// *actual* alpha over a checkerboard, so a transparent pick visibly reads as transparent instead of
// being flattened against a single opaque grey (the old Clay-quad preview did exactly that —
// composite_opaque() against one checker color — losing the transparency cue entirely). Same
// push-constant packing, rounded-corner masking, and output-linearization rationale as
// ui_color_picker_alpha_bar.slang; the only difference is alpha is a constant (the color's own)
// rather than a 0->1 ramp across the bar's width.

import sturdy_common;

struct PreviewConstants {
    float2 position;
    float2 size;
    float2 viewportSize;
    // See UI::CustomElement.hpp's UiElementConstants doc comment: _reserved0/_reserved1 complete the
    // shared prefix to a clean 32 bytes, so color below starts at byte 32 with no implicit compiler
    // gap to account for on the C++ packing side (UI::pack_gradient_shader_params).
    float _reserved0;
    float _reserved1;
    float4 color; // rgb = picked color, a = the picked color's own alpha.
    float4 params; // x = whole-part opacity (disabled dimming), y = checker cell edge in pixels, zw unused.
    float4 checkerLight;
    float4 checkerDark;
    float4 cornerRadius; // topLeft, topRight, bottomLeft, bottomRight.
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<PreviewConstants> constants;

#else

[[push_constant]] ConstantBuffer<PreviewConstants> constants;

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
    float alpha = clamp01(constants.color.a);
    float opacity = clamp01(constants.params.x);

    // Fixed small cells (ColorPickerStyle::checker_cell_size) — see the alpha bar shader's own
    // comment for why height-scaled cells read as chunky stripes rather than a transparency checker.
    float cellSize = max(constants.params.y, 1.0);
    float2 cell = floor(input.localPixelPos / cellSize);
    bool isLight = fmod(cell.x + cell.y, 2.0) < 1.0;
    float3 checker = isLight ? constants.checkerLight.rgb : constants.checkerDark.rgb;

    float3 composited = lerp(checker, constants.color.rgb, alpha);

    float2 halfSize = constants.size * 0.5;
    float2 centerPos = input.localPixelPos - halfSize;
    float dist = roundedBoxSDF(centerPos, halfSize, constants.cornerRadius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float mask = clamp01(0.5 - dist / edgeSoftness);

    return float4(srgbToLinear(composited), opacity * mask);
}
