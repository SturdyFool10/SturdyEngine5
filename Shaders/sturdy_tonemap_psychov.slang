// PsychoV tonemap operator: a Slang port of clshortfuse/renodx's "psychov" test22 psychovisual
// tonemapper (src/shaders/tonemap/psychov/test22.hlsl, psychotm_test22()), including the color-
// science it depends on (src/shaders/color/{rgb,macleod_boynton,gamut}.hlsl, src/shaders/math.hlsl).
//
//   Copyright (C) 2026 Carlos Lopez — https://github.com/clshortfuse/renodx
//   SPDX-License-Identifier: MIT
//
// Unlike a fixed-formula operator (Reinhard, AgX), PsychoV is an observer model: it converts scene
// color into Stockman-Sharpe LMS cone space, expresses it relative to an adapted background state,
// grades highlights/shadows and purity in MacLeod-Boynton chromaticity, compresses via a
// slope-normalized log-domain shoulder (auto-derived from simultaneous-contrast literature when
// `compression == 0`), restores MacLeod-Boynton hue direction plus an M-cone-crosstalk hue bias,
// and finally gamut-compresses against a chosen RGB hull using the CIE 170-2 human-visible-gamut
// boundary. See renodx's own psychotm_test22() doc comment for the full literature citations behind
// each stage — reproduced there, not duplicated here.
//
// Ported subset: this is renodx's *concrete* psychotm_test22() body, which does not itself call the
// bleaching / ON-OFF-pathway / divisive-normalization helpers its surrounding design-doc comment
// describes as the aspirational full observer cascade — those parameters (bleaching_intensity,
// clip_point, hue_restore, white_curve_mode, adaptive_normalization) are accepted by the upstream
// function signature but read nowhere in its body, so they carry no behavior to port and are omitted
// here (along with cone_response_exponent/adaptation_contrast, whose product upstream calls
// legacy_response_scale and which upstream itself marks a deprecated compatibility knob — always
// 1.0 here, i.e. a no-op, matching the neutral default).

import sturdy_common;

// ---------------------------------------------------------------------------------------------
// Safe division / sign helpers (renodx math.hlsl's DivideSafe/CopySign, reimplemented directly
// rather than ported byte-for-byte — same fallback-on-zero-divisor semantics).
// ---------------------------------------------------------------------------------------------

float psychovDivideSafe(float dividend, float divisor, float fallback) {
    return divisor == 0.0 ? fallback : dividend / divisor;
}

float3 psychovDivideSafe3(float3 dividend, float3 divisor, float3 fallback) {
    return float3(
        psychovDivideSafe(dividend.x, divisor.x, fallback.x),
        psychovDivideSafe(dividend.y, divisor.y, fallback.y),
        psychovDivideSafe(dividend.z, divisor.z, fallback.z));
}

float psychovCopySign(float mag, float sgn) { return sgn < 0.0 ? -abs(mag) : abs(mag); }
float3 psychovCopySign3(float3 mag, float3 sgn) {
    return float3(psychovCopySign(mag.x, sgn.x), psychovCopySign(mag.y, sgn.y), psychovCopySign(mag.z, sgn.z));
}

float3x3 renodxInvert3x3(float3x3 m) {
    float a = m[0][0], b = m[0][1], c = m[0][2];
    float d = m[1][0], e = m[1][1], f = m[1][2];
    float g = m[2][0], h = m[2][1], i = m[2][2];

    float A = e * i - f * h;
    float B = -(d * i - f * g);
    float C = d * h - e * g;
    float D = -(b * i - c * h);
    float E = a * i - c * g;
    float F = -(a * h - b * g);
    float G = b * f - c * e;
    float H = -(a * f - c * d);
    float I = a * e - b * d;

    float det = a * A + b * B + c * C;
    float invDet = psychovDivideSafe(1.0, det, 0.0);

    return float3x3(A, D, G, B, E, H, C, F, I) * invDet;
}

// ---------------------------------------------------------------------------------------------
// Colorimetry primaries/observer matrices (renodx color/rgb.hlsl), copied verbatim — HLSL and
// Slang share the same row-major float3x3(...)/mul(M,v) convention, so no transpose is needed.
// ---------------------------------------------------------------------------------------------

static const float3x3 PSYCHOV_BT709_TO_XYZ_MAT = float3x3(
    0.4123907993, 0.3575843394, 0.1804807884,
    0.2126390059, 0.7151686788, 0.0721923154,
    0.0193308187, 0.1191947798, 0.9505321522);

static const float3x3 PSYCHOV_XYZ_TO_BT709_MAT = float3x3(
    3.2409699419, -1.5373831776, -0.4986107603,
    -0.9692436363, 1.8759675015, 0.0415550574,
    0.0556300797, -0.2039769589, 1.0569715142);

static const float3x3 PSYCHOV_BT2020_TO_XYZ_MAT = float3x3(
    0.6369580483, 0.1446169036, 0.1688809752,
    0.2627002120, 0.6779980715, 0.0593017165,
    0.0000000000, 0.0280726930, 1.0609850577);

static const float3x3 PSYCHOV_XYZ_TO_BT2020_MAT = float3x3(
    1.7166511880, -0.3556707838, -0.2533662814,
    -0.6666843518, 1.6164812366, 0.0157685458,
    0.0176398574, -0.0427706133, 0.9421031212);

// Stockman-Sharpe 2-degree cone-fundamental fit (CVRL), CIE 170-2.
static const float3x3 PSYCHOV_XYZ_TO_LMS_MAT = float3x3(
    0.2670502842655792, 0.8471990148492798, -0.03470416612462053,
    -0.38706882411220156, 1.165429935890458, 0.10302286696614202,
    0.026727793989083093, -0.02729131667566509, 0.5333267257603284);

// CIE 170-2 Table 10.5 MacLeod-Boynton weights fit to the Stockman-Sharpe basis above.
static const float3 PSYCHOV_LMS_WEIGHTS = float3(0.68990272, 0.34832189, 0.0371597069161);

static const float2 PSYCHOV_WHITE_POINT_D65 = float2(0.31272, 0.32903);

// lms::from::BT709 / bt709::from::LMS (rgb.hlsl).
float3 psychovLmsFromBt709(float3 bt709) { return mul(PSYCHOV_XYZ_TO_LMS_MAT, mul(PSYCHOV_BT709_TO_XYZ_MAT, bt709)); }
float3 psychovBt709FromLms(float3 lms) { return mul(PSYCHOV_XYZ_TO_BT709_MAT, mul(renodxInvert3x3(PSYCHOV_XYZ_TO_LMS_MAT), lms)); }

// ---------------------------------------------------------------------------------------------
// MacLeod-Boynton weighted-LMS algebra (color/macleod_boynton.hlsl).
// ---------------------------------------------------------------------------------------------

float3 psychovWeighLms(float3 lms) { return lms * PSYCHOV_LMS_WEIGHTS; }
float3 psychovUnweighLms(float3 weightedLms) { return weightedLms / PSYCHOV_LMS_WEIGHTS; }

static const float3x3 PSYCHOV_XYZ_TO_LMS_WEIGHTED_MAT = float3x3(
    PSYCHOV_LMS_WEIGHTS.x * PSYCHOV_XYZ_TO_LMS_MAT[0][0], PSYCHOV_LMS_WEIGHTS.x * PSYCHOV_XYZ_TO_LMS_MAT[0][1], PSYCHOV_LMS_WEIGHTS.x * PSYCHOV_XYZ_TO_LMS_MAT[0][2],
    PSYCHOV_LMS_WEIGHTS.y * PSYCHOV_XYZ_TO_LMS_MAT[1][0], PSYCHOV_LMS_WEIGHTS.y * PSYCHOV_XYZ_TO_LMS_MAT[1][1], PSYCHOV_LMS_WEIGHTS.y * PSYCHOV_XYZ_TO_LMS_MAT[1][2],
    PSYCHOV_LMS_WEIGHTS.z * PSYCHOV_XYZ_TO_LMS_MAT[2][0], PSYCHOV_LMS_WEIGHTS.z * PSYCHOV_XYZ_TO_LMS_MAT[2][1], PSYCHOV_LMS_WEIGHTS.z * PSYCHOV_XYZ_TO_LMS_MAT[2][2]);

static const float3x3 PSYCHOV_BT709_TO_LMS_WEIGHTED_MAT = mul(PSYCHOV_XYZ_TO_LMS_WEIGHTED_MAT, PSYCHOV_BT709_TO_XYZ_MAT);
static const float3x3 PSYCHOV_BT2020_TO_LMS_WEIGHTED_MAT = mul(PSYCHOV_XYZ_TO_LMS_WEIGHTED_MAT, PSYCHOV_BT2020_TO_XYZ_MAT);

// Reconstruct weighted LMS from MB chromaticity (l, s) plus the carried MB scale term y = Lw + Mw.
float3 psychovWeightedLmsFromMb(float2 ls, float lPlusM) {
    return float3(ls.x, 1.0 - ls.x, ls.y) * lPlusM;
}

// Convert weighted LMS into MB (l, s, y).
float3 psychovMbFromWeightedLms(float3 weightedLms) {
    float weightedL = weightedLms.x;
    float weightedM = weightedLms.y;
    float yMb = max(weightedL + weightedM, 0.0);
    float inv = psychovDivideSafe(1.0, yMb, 0.0);
    return float3(weightedL * inv, weightedLms.z * inv, yMb);
}

float2 psychovD65WhiteChromaticity() {
    // from::XY(WHITE_POINT_D65): convert the D65 xy chromaticity to weighted-LMS MB coordinates.
    float2 xy = PSYCHOV_WHITE_POINT_D65;
    float3 xyz = float3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y); // xyY(xy, Y=1) -> XYZ
    float3 weightedLms = mul(PSYCHOV_XYZ_TO_LMS_WEIGHTED_MAT, xyz);
    return psychovMbFromWeightedLms(weightedLms).xy;
}

// ---------------------------------------------------------------------------------------------
// CIE 170-2 human-visible-gamut boundary + RGB-hull gamut compression (color/gamut.hlsl). Uses
// the fast 7-halfspace approximation (renodx's non-"Precise" path), which is what test22 uses.
// ---------------------------------------------------------------------------------------------

static const int PSYCHOV_CIE1702_EDGE_COUNT = 7;
static const float2 PSYCHOV_CIE1702_HALFSPACE_NORMALS[7] = {
    float2(-0.043889, -0.006807),
    float2(-0.007821, -0.008564),
    float2(-0.000604, -0.007942),
    float2(0.0, -0.080835),
    float2(0.953597, 0.307020),
    float2(-0.060969, 0.019752),
    float2(-0.106895, 0.004035),
};
static const float PSYCHOV_CIE1702_HALFSPACE_NUMERATORS[7] = {
    0.0065035249, 0.00104900495, 0.000207697044, 0.00165556648,
    0.252472349, 0.0241967351, 0.0199621232,
};
static const float PSYCHOV_RAY_T_MAX = 3.402823466e+38;

float psychovRayExitTCIE1702(float2 origin, float2 direction) {
    if (dot(direction, direction) <= 1.0e-14) {
        return PSYCHOV_RAY_T_MAX;
    }
    float2 whiteToOrigin = psychovD65WhiteChromaticity() - origin;
    float tBest = PSYCHOV_RAY_T_MAX;
    bool hitAny = false;
    for (int i = 0; i < PSYCHOV_CIE1702_EDGE_COUNT; ++i) {
        float2 halfspaceNormal = PSYCHOV_CIE1702_HALFSPACE_NORMALS[i];
        float denom = dot(halfspaceNormal, direction);
        float numerator = PSYCHOV_CIE1702_HALFSPACE_NUMERATORS[i] + dot(halfspaceNormal, whiteToOrigin);
        float t = denom > 1.0e-8 ? numerator / denom : PSYCHOV_RAY_T_MAX;
        tBest = min(tBest, t);
        hitAny = hitAny || (denom > 1.0e-8);
    }
    return hitAny ? max(tBest, 0.0) : PSYCHOV_RAY_T_MAX;
}

float psychovRayExitTCIE1702D(float2 direction) {
    return psychovRayExitTCIE1702(psychovD65WhiteChromaticity(), direction);
}

float psychovCross2(float2 a, float2 b) { return a.x * b.y - a.y * b.x; }

bool psychovRaySegmentHit2D(float2 origin, float2 direction, float2 a, float2 b, out float tHit) {
    tHit = 0.0;
    float2 edge = b - a;
    float denom = psychovCross2(direction, edge);
    if (abs(denom) <= 1.0e-20) {
        return false;
    }
    float2 aOrigin = a - origin;
    float t = psychovCross2(aOrigin, edge) / denom;
    float u = psychovCross2(aOrigin, direction) / denom;
    if (t < 0.0 || u < 0.0 || u > 1.0) {
        return false;
    }
    tHit = t;
    return true;
}

float2 psychovMbFromWeightedPrimary(float3 weightedLmsPrimary) {
    return psychovMbFromWeightedLms(weightedLmsPrimary).xy;
}

float3 psychovRgbToAdaptiveWeightedLmsPrimaryColumn(float3x3 rgbToLmsWeightedMat, int primaryIndex, float3 currentAdaptiveStateLms) {
    float3 column = float3(rgbToLmsWeightedMat[0][primaryIndex], rgbToLmsWeightedMat[1][primaryIndex], rgbToLmsWeightedMat[2][primaryIndex]);
    return column / currentAdaptiveStateLms;
}

void psychovMakeRgbTriangleInMbAdaptiveWeighted(float3x3 rgbToLmsWeightedMat, float3 currentAdaptiveStateLms, out float2 r, out float2 g, out float2 b) {
    r = psychovMbFromWeightedPrimary(psychovRgbToAdaptiveWeightedLmsPrimaryColumn(rgbToLmsWeightedMat, 0, currentAdaptiveStateLms));
    g = psychovMbFromWeightedPrimary(psychovRgbToAdaptiveWeightedLmsPrimaryColumn(rgbToLmsWeightedMat, 1, currentAdaptiveStateLms));
    b = psychovMbFromWeightedPrimary(psychovRgbToAdaptiveWeightedLmsPrimaryColumn(rgbToLmsWeightedMat, 2, currentAdaptiveStateLms));
}

float psychovRayMaxTRgbTriangleInMb(float2 origin, float2 direction, float2 r, float2 g, float2 b, out bool hasSolution) {
    hasSolution = false;
    if (dot(direction, direction) <= 1.0e-14) {
        return 0.0;
    }
    float tBest = PSYCHOV_RAY_T_MAX;
    float tHit;
    bool hitAny = false;
    if (psychovRaySegmentHit2D(origin, direction, r, g, tHit)) { tBest = min(tBest, tHit); hitAny = true; }
    if (psychovRaySegmentHit2D(origin, direction, g, b, tHit)) { tBest = min(tBest, tHit); hitAny = true; }
    if (psychovRaySegmentHit2D(origin, direction, b, r, tHit)) { tBest = min(tBest, tHit); hitAny = true; }
    hasSolution = hitAny;
    return hitAny ? max(tBest, 0.0) : 0.0;
}

float psychovNeutwoPeakClip(float x, float peak, float clipValue) {
    float peakSafe = max(peak, 0.0);
    float clipSafe = max(clipValue, peakSafe);
    float x2 = x * x;
    float clip2 = clipSafe * clipSafe;
    float peak2 = peakSafe * peakSafe;
    float denominatorSquared = x2 * (clip2 - peak2) + clip2 * peak2;
    return (clipSafe * peakSafe * x) * rsqrt(max(denominatorSquared, 1.0e-20));
}

float psychovNeutwoScaleFromRayT(float tPeak, float tClip) {
    float tPeakSafe = max(tPeak, 0.0);
    float tClipSafe = max(tClip, tPeakSafe);
    return saturate(psychovNeutwoPeakClip(1.0, tPeakSafe, tClipSafe));
}

float psychovSoftCompressionActivationFromRayT(float tPeak, float knee) {
    float outside = 1.0 - saturate(tPeak);
    return psychovDivideSafe(outside, outside + max(knee, 1.0e-6), 0.0);
}

float3 psychovClampWeightedLmsToCIE1702(float3 lmsWeightedInput) {
    float3 lmsWeightedClamped = max(lmsWeightedInput, 0.0);
    float3 mb = psychovMbFromWeightedLms(lmsWeightedClamped);
    float yMb = mb.z;
    float2 ls = mb.xy;
    if (!(yMb > 1.0e-20)) {
        return float3(lmsWeightedClamped.x, lmsWeightedClamped.y, 0.0);
    }
    float2 mbWhite = psychovD65WhiteChromaticity();
    float2 direction = ls - mbWhite;
    if (dot(direction, direction) <= 1.0e-14) {
        return lmsWeightedClamped;
    }
    float tClip = psychovRayExitTCIE1702D(direction);
    float2 lsOut = mbWhite + direction * min(1.0, tClip);
    return psychovWeightedLmsFromMb(lsOut, yMb);
}

float3 psychovGamutCompressWeightedLmsCoreRgbBound(float3 lmsWeightedInput, float2 boundR, float2 boundG, float2 boundB, float strength) {
    float3 lmsWeightedClamped = psychovClampWeightedLmsToCIE1702(max(lmsWeightedInput, 0.0));
    float3 mb = psychovMbFromWeightedLms(lmsWeightedClamped);
    float yMb = mb.z;
    float2 ls = mb.xy;
    if (!(yMb > 1.0e-20)) {
        return float3(lmsWeightedClamped.x, lmsWeightedClamped.y, 0.0);
    }
    float2 mbWhite = psychovD65WhiteChromaticity();
    float2 direction = ls - mbWhite;
    if (dot(direction, direction) <= 1.0e-14) {
        return lmsWeightedClamped;
    }
    bool hasPeak = false;
    float tPeak = psychovRayMaxTRgbTriangleInMb(mbWhite, direction, boundR, boundG, boundB, hasPeak);
    float tClip = psychovRayExitTCIE1702D(direction);
    if (!hasPeak) {
        tPeak = tClip;
    }
    float tHard = saturate(tPeak);
    float tSoft = psychovNeutwoScaleFromRayT(min(tPeak, tClip), tClip);
    float softMix = saturate(strength) * psychovSoftCompressionActivationFromRayT(tPeak, 0.08);
    float tFinal = lerp(tHard, tSoft, softMix);
    float2 lsOut = mbWhite + tFinal * direction;
    return psychovWeightedLmsFromMb(lsOut, yMb);
}

float3 psychovGamutCompressAdaptiveWeightedLmsBound(float3 lmsWeightedInput, float3 currentAdaptiveStateLms, float3x3 boundRgbToLmsWeightedMat, float strength) {
    float2 boundR, boundG, boundB;
    psychovMakeRgbTriangleInMbAdaptiveWeighted(boundRgbToLmsWeightedMat, currentAdaptiveStateLms, boundR, boundG, boundB);
    return psychovGamutCompressWeightedLmsCoreRgbBound(lmsWeightedInput, boundR, boundG, boundB, strength);
}

// ---------------------------------------------------------------------------------------------
// psychov-22-specific helpers (tonemap/psychov/test22.hlsl).
// ---------------------------------------------------------------------------------------------

float psychovYfFromLms(float3 lms) {
    float3 weightedLms = psychovWeighLms(lms);
    return max(weightedLms.x + weightedLms.y, 1.0e-6);
}

// h = (reference_range_log10 / 2) / log10(peak / anchor_out); reference range is Kunkel & Reinhard
// (APGV 2010)'s 3.7 log10-unit simultaneous-contrast dynamic range estimate.
float psychovAutoCompressionFromCenteredReferenceRange(float anchorOutYf, float peakYf) {
    float peakOverAnchor = psychovDivideSafe(max(peakYf, 1.0e-6), max(anchorOutYf, 1.0e-6), 1.0);
    peakOverAnchor = max(peakOverAnchor, 1.0 + 1.0e-6);
    static const float referenceOneSideRangeLog10 = 3.7 / 2.0;
    float actualAboveAdaptationRangeLog10 = max(log10(peakOverAnchor), 1.0e-6);
    float h = referenceOneSideRangeLog10 / actualAboveAdaptationRangeLog10;
    return max(h, 1.0);
}

float3 psychovToAdaptiveRelativeWeightedLms(float3 lmsInput, float3 currentAdaptiveStateLms) {
    return psychovDivideSafe3(psychovWeighLms(lmsInput), currentAdaptiveStateLms, float3(0.0, 0.0, 0.0));
}

float3 psychovFromAdaptiveRelativeWeightedLms(float3 lmsWeightedRelative, float3 currentAdaptiveStateLms) {
    return lmsWeightedRelative * max(currentAdaptiveStateLms, float3(1.0e-6, 1.0e-6, 1.0e-6));
}

float3 psychovAdaptiveRelativeWeightedNeutral() {
    return psychovWeighLms(float3(1.0, 1.0, 1.0));
}

// CastleCSF-style adapted cone contrast (ACC): Ach = dLw+dMw, RG = dLw - mc1*dMw, YV = -dLw-dMw+mc2*dSw.
float3 psychovAccFromAdaptiveRelativeWeightedDelta(float3 deltaLmsW) {
    float3 neutralW = psychovAdaptiveRelativeWeightedNeutral();
    float mc1 = psychovDivideSafe(neutralW.x, neutralW.y, 0.0);
    float mc2 = psychovDivideSafe(neutralW.x + neutralW.y, neutralW.z, 0.0);
    return float3(
        deltaLmsW.x + deltaLmsW.y,
        deltaLmsW.x - mc1 * deltaLmsW.y,
        -deltaLmsW.x - deltaLmsW.y + mc2 * deltaLmsW.z);
}

float psychovPurpleComplementaryGateFromAcc(float3 acc) {
    float rg = acc.y;
    float yv = acc.z;
    float chroma = max(length(acc.yz), 1.0e-6);
    float rgPos = saturate(rg / chroma);
    float yvPos = saturate(yv / chroma);
    float mixed = saturate(min(abs(rg), abs(yv)) / max(max(abs(rg), abs(yv)), 1.0e-6));
    return saturate(2.0 * rgPos * yvPos * mixed);
}

float3 psychovApplyAdaptiveMbPurity(float3 lmsInput, float3 adaptiveNeutralLms, float purityDelta) {
    if (abs(purityDelta - 1.0) <= 1.0e-5) {
        return lmsInput;
    }
    float3 relativeWeighted = psychovToAdaptiveRelativeWeightedLms(lmsInput, adaptiveNeutralLms);
    float3 mb = psychovMbFromWeightedLms(relativeWeighted);
    float3 mbNeutral = psychovMbFromWeightedLms(psychovWeighLms(float3(1.0, 1.0, 1.0)));
    float2 mbScaledXy = lerp(mbNeutral.xy, mb.xy, purityDelta);
    float3 relativeWeightedOut = psychovWeightedLmsFromMb(mbScaledXy, mb.z);
    return psychovUnweighLms(psychovFromAdaptiveRelativeWeightedLms(relativeWeightedOut, adaptiveNeutralLms));
}

// ---------------------------------------------------------------------------------------------
// Entry point: a faithful port of psychotm_test22(). `exposure` is intentionally not a parameter —
// this engine's shared exposure/white_point scaling is already applied to `bt709LinearInput`
// before this is called (see fullscreen_tonemap.slang), matching every other operator here.
// `peakValue` is the caller's real output headroom in reference-white multiples: 1.0 for SDR
// output, hdr_peak_nits/hdr_paper_white_nits for HDR output — see that file for why.
// ---------------------------------------------------------------------------------------------

float3 psychovTonemap(
    float3 bt709LinearInput,
    float peakValue,
    float highlights,
    float shadows,
    float contrast,
    float puritySc,
    float gamutCompression,
    uint gamutCompressionUseBt2020,
    float compressionParam,
    float3 currentAdaptiveStateBt709,
    float3 currentBackgroundStateBt709) {
    float3 lmsIn = psychovLmsFromBt709(bt709LinearInput);
    float3 lmsPeak = psychovLmsFromBt709(float3(peakValue, peakValue, peakValue));
    float3 currentAdaptiveStateLms = psychovLmsFromBt709(currentAdaptiveStateBt709);
    float3 desiredBackgroundStateLms = psychovLmsFromBt709(currentBackgroundStateBt709);

    float3 lmsCones = lmsIn;
    float3 anchorIn = max(currentAdaptiveStateLms, float3(1.0e-6, 1.0e-6, 1.0e-6));
    float3 anchorOut = max(desiredBackgroundStateLms, float3(1.0e-6, 1.0e-6, 1.0e-6));

    float contrastPower = max(contrast, 1.0e-6);

    // Per-cone highlight/shadow grading (psychov17-style, kept in the all-LMS path).
    float3 gradedLms = abs(lmsCones);
    if (highlights != 1.0) {
        float3 ratioAbs = abs(psychovDivideSafe3(gradedLms, anchorIn, float3(0.0, 0.0, 0.0)));
        if (highlights > 1.0) {
            gradedLms = max(gradedLms, lerp(gradedLms, anchorIn * pow(ratioAbs, float3(highlights, highlights, highlights)), gradedLms));
        } else {
            gradedLms = min(gradedLms, psychovDivideSafe3(gradedLms, 1.0 + anchorIn * pow(ratioAbs, float3(2.0 - highlights, 2.0 - highlights, 2.0 - highlights)) - gradedLms, float3(0.0, 0.0, 0.0)));
        }
    }
    if (shadows != 1.0) {
        float3 ratioAbs = abs(psychovDivideSafe3(gradedLms, anchorIn, float3(0.0, 0.0, 0.0)));
        if (shadows > 1.0) {
            gradedLms = max(gradedLms, gradedLms * (1.0 + psychovDivideSafe3(gradedLms * anchorIn, pow(ratioAbs, float3(shadows, shadows, shadows)), float3(0.0, 0.0, 0.0))));
        } else {
            gradedLms = clamp(
                gradedLms * (1.0 - psychovDivideSafe3(gradedLms * anchorIn, pow(ratioAbs, float3(2.0 - shadows, 2.0 - shadows, 2.0 - shadows)), float3(0.0, 0.0, 0.0))),
                float3(0.0, 0.0, 0.0),
                gradedLms);
        }
    }

    // Purity delta: purity_scale/contrast applied in MacLeod-Boynton space before the shoulder.
    float purityDelta = psychovDivideSafe(max(puritySc, 1.0e-6), contrastPower, 1.0);
    float3 contrastInput = psychovApplyAdaptiveMbPurity(gradedLms, anchorIn, purityDelta);
    float3 responseSourceLms = psychovCopySign3(contrastInput, lmsCones);

    // Slope-normalized compression shoulder; compression==0 auto-derives h from the simultaneous-
    // contrast reference range.
    float h = compressionParam;
    if (compressionParam == 0.0) {
        h = psychovAutoCompressionFromCenteredReferenceRange(psychovYfFromLms(anchorOut), psychovYfFromLms(lmsPeak));
    }
    h = max(h, 1.0e-6);
    float hRcp = 1.0 / h;

    float3 anchorOverPeak = anchorOut / max(lmsPeak, float3(1.0e-6, 1.0e-6, 1.0e-6));
    float3 slopeNorm = 1.0 - pow(max(anchorOverPeak, float3(1.0e-6, 1.0e-6, 1.0e-6)), float3(h, h, h));
    float3 n = contrastPower / max(slopeNorm, float3(1.0e-6, 1.0e-6, 1.0e-6));
    float3 q = pow(max(contrastInput / anchorIn, float3(1.0e-6, 1.0e-6, 1.0e-6)), n * h);
    float3 shoulder = pow(max(lmsPeak / anchorOut, float3(1.0e-6, 1.0e-6, 1.0e-6)), float3(h, h, h)) - 1.0;
    float3 saturated = lmsPeak * pow(q / max(q + shoulder, float3(1.0e-6, 1.0e-6, 1.0e-6)), float3(hRcp, hRcp, hRcp));
    float3 displayScaled = psychovCopySign3(saturated, responseSourceLms);

    float3 displayScaledRelativeWeighted = psychovToAdaptiveRelativeWeightedLms(displayScaled, currentAdaptiveStateLms);
    float3 sourceRelativeW = psychovToAdaptiveRelativeWeightedLms(responseSourceLms, currentAdaptiveStateLms);

    float3 neutralW = psychovAdaptiveRelativeWeightedNeutral();
    float neutralYf = max(neutralW.x + neutralW.y, 1.0e-6);
    float2 mbNeutralXy = neutralW.xz / neutralYf;

    // psychov17-style hue restore: keep display target radius/y_MB, blend hue direction with source.
    {
        float2 sourceOffset = sourceRelativeW.xz * psychovDivideSafe(1.0, max(sourceRelativeW.x + sourceRelativeW.y, 0.0), 0.0) - mbNeutralXy;
        float targetY = max(displayScaledRelativeWeighted.x + displayScaledRelativeWeighted.y, 0.0);
        float2 targetOffset = displayScaledRelativeWeighted.xz * psychovDivideSafe(1.0, targetY, 0.0) - mbNeutralXy;
        float targetRadius2 = dot(targetOffset, targetOffset);
        float targetRadius = targetRadius2 * rsqrt(max(targetRadius2, 1.0e-7));
        float2 blendedOffset = lerp(targetOffset, sourceOffset * (targetRadius * rsqrt(max(dot(sourceOffset, sourceOffset), 1.0e-7))), 0.5);
        blendedOffset *= targetRadius * rsqrt(max(dot(blendedOffset, blendedOffset), 1.0e-7));
        displayScaledRelativeWeighted = psychovWeightedLmsFromMb(mbNeutralXy + blendedOffset, targetY);
    }

    // M-cone crosstalk as an MB hue-direction bias (suppresses magenta/purple over-rotation).
    {
        float3 sourceAcc = psychovAccFromAdaptiveRelativeWeightedDelta(sourceRelativeW - neutralW) / neutralYf;
        float3 drive = abs(responseSourceLms) / max(currentAdaptiveStateLms, float3(1.0e-6, 1.0e-6, 1.0e-6));
        float lOverM = max(drive.x - drive.y, 0.0);
        float sOverM = max(drive.z - drive.y, 0.0);
        float lsMixedShare = psychovDivideSafe(min(lOverM, sOverM), max(lOverM + sOverM, 1.0e-6), 0.0);
        float coneComplementaryGate = smoothstep(0.02, 0.15, lsMixedShare);
        float accComplementaryGate = psychovPurpleComplementaryGateFromAcc(sourceAcc);
        float spectralConfidence = 1.0 - max(coneComplementaryGate, accComplementaryGate);
        float lmShare = psychovDivideSafe(drive.y, max(drive.x + drive.y, 1.0e-6), 0.0);
        float lBias = saturate(lOverM / max(drive.x, 1.0e-6)) * saturate(lmShare / 0.25);
        float sBias = 0.15 * saturate(sOverM / max(drive.z, 1.0e-6));
        float mBiasWeight = spectralConfidence * (0.12 * lBias + 0.025 * sBias);

        float targetY = max(displayScaledRelativeWeighted.x + displayScaledRelativeWeighted.y, 0.0);
        float2 targetOffset = displayScaledRelativeWeighted.xz * psychovDivideSafe(1.0, targetY, 0.0) - mbNeutralXy;
        float targetRadius2 = dot(targetOffset, targetOffset);
        float targetRadius = targetRadius2 * rsqrt(max(targetRadius2, 1.0e-7));
        float2 mOffset = -mbNeutralXy;
        float2 bentOffset = lerp(targetOffset, mOffset * (targetRadius * rsqrt(max(dot(mOffset, mOffset), 1.0e-7))), saturate(mBiasWeight));
        bentOffset *= targetRadius * rsqrt(max(dot(bentOffset, bentOffset), 1.0e-7));
        displayScaledRelativeWeighted = psychovWeightedLmsFromMb(mbNeutralXy + bentOffset, targetY);
    }

    if (gamutCompression != 0.0) {
        float3x3 boundMat = gamutCompressionUseBt2020 != 0u ? PSYCHOV_BT2020_TO_LMS_WEIGHTED_MAT : PSYCHOV_BT709_TO_LMS_WEIGHTED_MAT;
        displayScaledRelativeWeighted = psychovGamutCompressAdaptiveWeightedLmsBound(displayScaledRelativeWeighted, currentAdaptiveStateLms, boundMat, gamutCompression);
    }

    return psychovBt709FromLms(psychovUnweighLms(psychovFromAdaptiveRelativeWeightedLms(displayScaledRelativeWeighted, currentAdaptiveStateLms)));
}
