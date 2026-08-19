// Per-pixel hue spectrum gradient for UI::color_picker() (UI/ColorPicker.hpp) — replaces a strip of
// up to 64 flat-colored Clay elements approximating the rainbow with a single custom_element() draw
// that computes hsvToRgb(hue, 1, 1) analytically at every pixel. See ui_color_picker_sv_plane.slang
// for the push-constant layout/packing rationale and the rounded-corner masking rationale (this
// shader's own container can be rounded via UI::ColorPickerComposition::hue.alter_decl, and a plain
// custom_element() draw is otherwise always sharp-cornered regardless of that).

import sturdy_common;

struct HueBarConstants {
    float2 position;
    float2 size;
    float2 viewportSize;
    // See UI::CustomElement.hpp's UiElementConstants doc comment: _reserved0/_reserved1 complete the
    // shared prefix to a clean 32 bytes, so params below starts at byte 32 with no implicit compiler
    // gap to account for on the C++ packing side (UI::pack_gradient_shader_params).
    float _reserved0;
    float _reserved1;
    float4 params; // x = opacity (0..1), yzw unused.
    float4 cornerRadius; // topLeft, topRight, bottomLeft, bottomRight.
};

[[push_constant]] ConstantBuffer<HueBarConstants> constants;

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
    float u = constants.size.x > 0.0 ? clamp01(input.localPixelPos.x / constants.size.x) : 0.0;
    float opacity = clamp01(constants.params.x);

    float2 halfSize = constants.size * 0.5;
    float2 centerPos = input.localPixelPos - halfSize;
    float dist = roundedBoxSDF(centerPos, halfSize, constants.cornerRadius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float mask = clamp01(0.5 - dist / edgeSoftness);

    // Linearized for the *Srgb render target — see ui_color_picker_sv_plane.slang's own comment on
    // this exact line for why returning hsvToRgb() raw double-encodes against the Clay quad path.
    return float4(srgbToLinear(hsvToRgb(u * 360.0, 1.0, 1.0)), opacity * mask);
}
