// Camera lens (vertex-warp fisheye), layered on top of whichever `sturdy_space` module is active: the functions
// below call `sturdySpaceWorldToClip` / `sturdySpaceClipToWorldH`, so a replacement space model keeps working
// and simply gains the lens. Kept out of `sturdy_space` itself so replacement modules need not define it.
import sturdy_space;

// ---------------------------------------------------------------------------------------------------------
// Camera lens (vertex-warp fisheye)
//
// The frame's projection is widened by S = 1 + strength (the renderer does that), and the clip position the
// *camera* passes output is then warped radially so the widened image is compressed back into the frame:
//     u_out = S u / (1 + (S - 1) u^2),   u = |ndc| / sqrt(2)  (0 at the centre, 1 at the corners)
// This is 1:1 with the un-widened projection at the centre (derivative S on a 1/S-wide image), squeezes the
// edges, and maps the corners exactly onto the frame corners. Beyond the corner radius the curve continues
// linearly so geometry outside the widened frustum keeps moving outward instead of folding back in.
// `sturdySpaceCameraClipToWorld*` undo it for every pass that reads depth back.
//
// The strength travels with each camera draw in the otherwise-unused w column of the (affine) model matrix
// (`model[0][3]`); it is 0 for shadow passes and everything else, which makes the warp the identity there.
// Row-vector convention like the rest of this module.
// ---------------------------------------------------------------------------------------------------------

/// The lens strength a camera draw carries in its model matrix (0 = no lens).
float sturdySpaceLensStrength(float4x4 model)
{
    return model[0][3];
}

/// Widened-projection ndc.xy -> screen ndc.xy.
float2 sturdyLensToScreen(float2 wideNdc, float strength)
{
    float s = 1.0 + strength;
    float u = sqrt(dot(wideNdc, wideNdc) * 0.5);
    float scale;
    if (u <= 1.0) {
        scale = s / (1.0 + (s - 1.0) * u * u);
    } else {
        float slope = (2.0 - s) / s;
        scale = (1.0 + (u - 1.0) * slope) / u;
    }
    return wideNdc * scale;
}

/// Screen ndc.xy -> widened-projection ndc.xy (the exact inverse of `sturdyLensToScreen`).
float2 sturdyLensToWide(float2 screenNdc, float strength)
{
    float s = 1.0 + strength;
    float uo = sqrt(dot(screenNdc, screenNdc) * 0.5);
    float scale;
    if (uo <= 1.0) {
        scale = 2.0 / (s + sqrt(max(s * s - 4.0 * (s - 1.0) * uo * uo, 0.0)));
    } else {
        float slope = max((2.0 - s) / s, 1.0e-3);
        scale = (1.0 + (uo - 1.0) / slope) / uo;
    }
    return screenNdc * scale;
}

/// Warps a clip position produced by the widened projection. Points at or behind the camera plane are left alone
/// (the direction to them is undefined there, and the hardware clips them anyway).
float4 sturdyLensWarpClip(float4 clip, float strength)
{
    if (strength <= 0.0 || clip.w <= 1.0e-5) return clip;
    float2 ndc = clip.xy / clip.w;
    return float4(sturdyLensToScreen(ndc, strength) * clip.w, clip.z, clip.w);
}

/// World position -> clip position for the *camera*, with the lens applied when `lensStrength > 0`.
float4 sturdySpaceCameraWorldToClip(float3 worldPosition, float4x4 viewProjection, float lensStrength)
{
    return sturdyLensWarpClip(sturdySpaceWorldToClip(worldPosition, viewProjection), lensStrength);
}

/// Screen-space (ndc.xy, depth, 1) -> homogeneous world position for the camera, undoing the lens.
float4 sturdySpaceCameraClipToWorldH(float4 clip, float4x4 inverseViewProjection, float lensStrength)
{
    if (lensStrength > 0.0) clip.xy = sturdyLensToWide(clip.xy, lensStrength);
    return sturdySpaceClipToWorldH(clip, inverseViewProjection);
}

/// Screen-space (ndc.xy, depth, 1) -> world position for the camera, undoing the lens.
float3 sturdySpaceCameraClipToWorld(float4 clip, float4x4 inverseViewProjection, float lensStrength)
{
    if (lensStrength > 0.0) clip.xy = sturdyLensToWide(clip.xy, lensStrength);
    return sturdySpaceClipToWorld(clip, inverseViewProjection);
}
