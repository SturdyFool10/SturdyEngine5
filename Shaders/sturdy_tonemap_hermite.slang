// Hermite-spline filmic tonemap operator.
//
// A toe/linear-midtone/shoulder response curve, each segment a cubic Hermite interpolant between
// two explicit (position, tangent) control points — the same toe+shoulder shape John Hable's
// piecewise filmic curves and Unity's legacy "Custom" tonemapping curve use, built here directly
// from the Hermite basis functions rather than a solved closed-form power curve, so every knob maps
// to one specific control point or tangent:
//
//   toe:      P0 = (0, 0),           tangent m0 = 1 - toeStrength
//             P1 = (toeLength, toeLength), tangent m1 = 1   (hands off to the identity midtone)
//   midtone:  y = x, from toeLength to (1 - shoulderLength)                 (always slope 1)
//   shoulder: P2 = (1 - shoulderLength, 1 - shoulderLength), tangent m2 = 1
//             P3 = (whiteClip, 1),    tangent m3 = shoulderAngle
//
// whiteClip = 2^shoulderStrength is how many stops above reference white the shoulder compresses
// before hard-clipping to 1.0 (SDR) — see hermiteTonemapHdr for how HDR output continues past it.

import sturdy_common;

float hermiteH00(float t) { return (2.0 * t - 3.0) * t * t + 1.0; }
float hermiteH10(float t) { return ((t - 2.0) * t + 1.0) * t; }
float hermiteH01(float t) { return (-2.0 * t + 3.0) * t * t; }
float hermiteH11(float t) { return (t - 1.0) * t * t; }

float hermiteSegment(float t, float p0, float m0, float p1, float m1, float segmentLength) {
    return hermiteH00(t) * p0 + hermiteH10(t) * m0 * segmentLength +
           hermiteH01(t) * p1 + hermiteH11(t) * m1 * segmentLength;
}

// Evaluates the scalar toe/linear/shoulder curve described above. Returns a value in [0, 1] for
// x in [0, whiteClip], and exactly 1.0 for x >= whiteClip (SDR hard clip) unless the caller wants
// the HDR continuation instead — see hermiteToneCurveHdr.
float hermiteToneCurve(float x, float toeStrength, float toeLength, float shoulderStrength, float shoulderLength, float shoulderAngle) {
    float toeLen = clamp(toeLength, 1.0e-4, 0.999);
    float shoulderLen = clamp(shoulderLength, 1.0e-4, 0.999 - toeLen);
    float midStart = toeLen;
    float midEnd = 1.0 - shoulderLen;
    float whiteClip = exp2(max(shoulderStrength, 0.0));

    if (x <= 0.0) {
        return 0.0;
    }
    if (x < midStart) {
        float t = x / midStart;
        return hermiteSegment(t, 0.0, 1.0 - saturate(toeStrength), midStart, 1.0, midStart);
    }
    if (x <= midEnd) {
        return x; // Identity midtone, slope 1 at both ends by construction.
    }
    if (x < whiteClip) {
        float shoulderSpan = max(whiteClip - midEnd, 1.0e-4);
        float t = (x - midEnd) / shoulderSpan;
        return hermiteSegment(t, midEnd, 1.0, 1.0, saturate(shoulderAngle), shoulderSpan);
    }
    return 1.0;
}

// HDR continuation: past whiteClip, extend the shoulder's own exit tangent (shoulderAngle) into an
// asymptotic approach toward headroomCeiling (hdr_peak_nits / hdr_paper_white_nits) rather than
// hard-clipping — a Michaelis-Menten-style extension chosen to match the shoulder's exact slope at
// the handoff point (C1-continuous), not a value taken from any specific filmic-curve reference.
float hermiteToneCurveHdr(float x, float toeStrength, float toeLength, float shoulderStrength, float shoulderLength, float shoulderAngle, float headroomCeiling) {
    float whiteClip = exp2(max(shoulderStrength, 0.0));
    if (x < whiteClip) {
        return hermiteToneCurve(x, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle);
    }
    float headroom = max(headroomCeiling - 1.0, 1.0e-4);
    float exitTangent = max(saturate(shoulderAngle), 1.0e-4);
    float t = (x - whiteClip) / headroom;
    return 1.0 + headroom * (exitTangent * t) / (1.0 + exitTangent * t);
}

float3 hermiteTonemap(float3 hdr, float toeStrength, float toeLength, float shoulderStrength, float shoulderLength, float shoulderAngle) {
    float3 x = max(hdr, 0.0);
    return float3(
        hermiteToneCurve(x.r, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle),
        hermiteToneCurve(x.g, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle),
        hermiteToneCurve(x.b, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle));
}

// Returns the same reference-domain units as hermiteTonemap() (1.0 = paper white), just permitted
// to exceed 1.0 up to headroomCeiling (hdr_peak_nits / hdr_paper_white_nits) instead of hard-
// clipping there — the caller (fullscreen_tonemap.slang) applies the actual nits scale + PQ encode
// uniformly for every operator, so this function does not multiply by nits itself.
float3 hermiteTonemapHdr(float3 hdr, float toeStrength, float toeLength, float shoulderStrength, float shoulderLength, float shoulderAngle, float headroomCeiling) {
    float3 x = max(hdr, 0.0);
    return float3(
        hermiteToneCurveHdr(x.r, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle, headroomCeiling),
        hermiteToneCurveHdr(x.g, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle, headroomCeiling),
        hermiteToneCurveHdr(x.b, toeStrength, toeLength, shoulderStrength, shoulderLength, shoulderAngle, headroomCeiling));
}
