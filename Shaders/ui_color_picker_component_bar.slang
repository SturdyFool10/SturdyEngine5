// Per-pixel gradient for one component slider of UI::color_picker() (UI/ColorPicker.hpp): sweeps
// that single component across its range while holding the other two at their current values — the
// "what happens if only this slider moves" preview, computed exactly through sturdy_common's
// color-space library (the GPU mirror of Foundation::Color), not approximated by stops. Same
// push-constant packing, rounded-corner masking, and linear-output rationale as
// ui_color_picker_sv_plane.slang. Space/component indices match UI::ColorPickerColorSpace and
// UI::color_picker_components() order exactly.

import sturdy_common;

struct ComponentBarConstants {
    float2 position;
    float2 size;
    float2 viewportSize;
    float4 params;          // x = color-space index, y = component index (0..2), z = minimum, w = maximum.
    float4 componentValues; // xyz = the three current components in the space's own units, w = opacity.
    float4 cornerRadius;    // topLeft, topRight, bottomLeft, bottomRight.
};

[[push_constant]] ConstantBuffer<ComponentBarConstants> constants;

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

float3 spaceComponentsToLinearRgb(int space, float c0, float c1, float c2) {
    LinearColor lin;
    switch (space) {
        case 0: { SrgbColor c; c.r = c0; c.g = c1; c.b = c2; c.a = 1.0; lin = srgbColorToLinearColor(c); break; }
        case 1: lin = makeLinearColor(c0, c1, c2, 1.0); break;
        case 2: { XyzColor c; c.x = c0; c.y = c1; c.z = c2; c.alpha = 1.0; lin = xyzToLinearColor(c); break; }
        case 3: { AdobeRgbColor c; c.r = c0; c.g = c1; c.b = c2; c.a = 1.0; lin = adobeRgbToLinearColor(c); break; }
        case 4: { DisplayP3Color c; c.r = c0; c.g = c1; c.b = c2; c.a = 1.0; lin = displayP3ToLinearColor(c); break; }
        case 5: { Rec2020Color c; c.r = c0; c.g = c1; c.b = c2; c.a = 1.0; lin = rec2020ToLinearColor(c); break; }
        case 6: { HslColor c; c.h = c0; c.s = c1; c.l = c2; c.a = 1.0; lin = hslToLinearColor(c); break; }
        case 7: { HsvColor c; c.h = c0; c.s = c1; c.v = c2; c.a = 1.0; lin = hsvToLinearColor(c); break; }
        case 8: { HwbColor c; c.h = c0; c.w = c1; c.b = c2; c.a = 1.0; lin = hwbToLinearColor(c); break; }
        case 9: { LabColor c; c.l = c0; c.a = c1; c.b = c2; c.alpha = 1.0; lin = labToLinearColor(c); break; }
        case 10: { LchColor c; c.l = c0; c.c = c1; c.h = c2; c.a = 1.0; lin = lchToLinearColor(c); break; }
        case 11: { LuvColor c; c.l = c0; c.u = c1; c.v = c2; c.alpha = 1.0; lin = luvToLinearColor(c); break; }
        case 12: { OklabColor c; c.l = c0; c.a = c1; c.b = c2; c.alpha = 1.0; lin = oklabToLinearColor(c); break; }
        case 13: { OklchColor c; c.l = c0; c.c = c1; c.h = c2; c.alpha = 1.0; lin = oklchToLinearColor(c); break; }
        default: lin = makeLinearColor(0.0, 0.0, 0.0, 1.0); break;
    }
    return float3(lin.r, lin.g, lin.b);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float t = constants.size.x > 0.0 ? clamp01(input.localPixelPos.x / constants.size.x) : 0.0;
    float swept = lerp(constants.params.z, constants.params.w, t);

    float c0 = constants.componentValues.x;
    float c1 = constants.componentValues.y;
    float c2 = constants.componentValues.z;
    int componentIndex = int(constants.params.y + 0.5);
    if (componentIndex == 0) { c0 = swept; } else if (componentIndex == 1) { c1 = swept; } else { c2 = swept; }

    // Out-of-gamut sweep positions clamp per channel, matching how the CPU side clamps the picked
    // color itself — clamp01 commutes with the (monotonic) sRGB encode, so this equals clamping in
    // display space. Already linear: *ToLinearColor lands directly in what the *Srgb target wants.
    float3 lin = clamp01(spaceComponentsToLinearRgb(int(constants.params.x + 0.5), c0, c1, c2));

    float2 halfSize = constants.size * 0.5;
    float2 centerPos = input.localPixelPos - halfSize;
    float dist = roundedBoxSDF(centerPos, halfSize, constants.cornerRadius);
    float edgeSoftness = max(fwidth(dist), SFT_EPSILON);
    float mask = clamp01(0.5 - dist / edgeSoftness);

    return float4(lin, clamp01(constants.componentValues.w) * mask);
}
