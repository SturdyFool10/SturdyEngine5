// Per-pixel alpha gradient over a checkerboard for UI::color_picker() (UI/ColorPicker.hpp) —
// replaces a strip of flat-colored Clay elements (one checker-composited cell per segment) with a
// single custom_element() draw that computes the checker pattern and the alpha lerp analytically at
// every pixel. See ui_color_picker_sv_plane.slang for the push-constant packing rationale and the
// rounded-corner masking rationale (this shader's own container can be rounded via
// UI::ColorPickerComposition::alpha.alter_decl, and a plain custom_element() draw is otherwise
// always sharp-cornered regardless of that).

import sturdy_common;

struct AlphaBarConstants {
    float2 position;
    float2 size;
    float2 viewportSize;
    float4 foregroundAndOpacity; // rgb = the picker's current color at full alpha, a = opacity.
    float4 checkerLight;
    float4 checkerDark;
    float4 cornerRadius; // topLeft, topRight, bottomLeft, bottomRight.
};

[[push_constant]] ConstantBuffer<AlphaBarConstants> constants;

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
    float alpha = constants.size.x > 0.0 ? clamp01(input.localPixelPos.x / constants.size.x) : 0.0;
    float opacity = clamp01(constants.foregroundAndOpacity.a);

    // A checker cell edge equal to the bar's own height gives square cells regardless of the bar's
    // length, the same "checker scale follows the strip thickness" convention most color pickers use.
    float cellSize = max(constants.size.y, 1.0);
    float2 cell = floor(input.localPixelPos / cellSize);
    bool isLight = fmod(cell.x + cell.y, 2.0) < 1.0;
    float3 checker = isLight ? constants.checkerLight.rgb : constants.checkerDark.rgb;

    float3 composited = lerp(checker, constants.foregroundAndOpacity.rgb, alpha);

    float2 halfSize = constants.size * 0.5;
    float2 centerPos = input.localPixelPos - halfSize;
    float dist = roundedBoxSDF(centerPos, halfSize, constants.cornerRadius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float mask = clamp01(0.5 - dist / edgeSoftness);

    return float4(composited, opacity * mask);
}
