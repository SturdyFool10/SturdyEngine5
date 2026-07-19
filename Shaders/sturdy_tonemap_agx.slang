// AgX tonemap operator (Troy Sobotka's AgX, shipped as Blender 4.0's default view transform).
//
// This is the widely-shared "AgX minimal" real-time approximation of Blender's OCIO AgX config:
// fixed inset matrix -> log2 encode over a fixed EV window -> a 6th-order polynomial fit of the
// actual AgX sigmoid -> fixed outset matrix -> optional ASC-CDL-style "look" grade (Punchy/Golden,
// the same two extra looks Blender itself ships). The matrices/polynomial/look constants below are
// the values that have circulated openly across the Bevy/Godot/three.js/shader community ports of
// Blender's AgX config since its 4.0 release.
//
// AgX's sigmoid is defined to saturate at 1.0 by construction (it is an SDR display transform,
// not a headroom-preserving operator) — see agxTonemapHdr's doc comment for how this engine
// extends it for HDR output.

import sturdy_common;

static const float3x3 AGX_INSET_MAT = float3x3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);

static const float3x3 AGX_OUTSET_MAT = float3x3(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

static const float AGX_MIN_EV = -12.47393;
static const float AGX_MAX_EV = 4.026069;

// Mean squared error ~3.67e-6 against the real AgX sigmoid over its designed input domain.
float3 agxContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Returns the SDR-domain result in [0, 1], already display-referred (linearized back out via the
// pow(2.2) EOTF step below so the caller's existing "write linear, let the *Srgb swapchain format
// encode it" convention still applies — AgX's own sigmoid otherwise leaves the result in a
// gamma-2.2-ish encoded space, since that's what its fit was solved against).
float3 agxTonemap(float3 hdr) {
    float3 v = mul(AGX_INSET_MAT, max(hdr, 0.0));
    v = clamp(log2(max(v, 1.0e-10)), AGX_MIN_EV, AGX_MAX_EV);
    v = (v - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV);
    v = saturate(agxContrastApprox(v));
    v = mul(AGX_OUTSET_MAT, v);
    v = pow(saturate(v), float3(2.2, 2.2, 2.2));
    return v;
}

float3 agxLook(float3 value, uint look) {
    static const float3 LUMA_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(value, LUMA_WEIGHTS);

    float3 slope = float3(1.0, 1.0, 1.0);
    float3 power = float3(1.0, 1.0, 1.0);
    float saturationScale = 1.0;
    if (look == 1) { // Punchy
        power = float3(1.35, 1.35, 1.35);
        saturationScale = 1.4;
    } else if (look == 2) { // Golden
        slope = float3(1.0, 0.9, 0.5);
        power = float3(0.8, 0.8, 0.8);
        saturationScale = 0.8;
    }

    float3 graded = pow(max(value * slope, 0.0), power);
    return max(luma + saturationScale * (graded - luma), 0.0);
}

// AgX's own curve has no defined headroom above 1.0 (it is fundamentally an SDR display transform
// with a fixed, saturating EV window), so this same agxTonemap()+agxLook() result is used for both
// SDR and HDR output — "HDR output" for AgX means presenting the same SDR-equivalent result at the
// display's actual paper-white brightness (fullscreen_tonemap.slang's shared final encode step),
// not inventing extra highlight detail AgX's algorithm doesn't have to give.
