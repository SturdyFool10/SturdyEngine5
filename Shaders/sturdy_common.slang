// Shared Slang utility library for Sturdy shaders.
//
// Usage from any shader under project_root/Shaders/*:
//     import sturdy_common;
//
// This module is dependency-free and contains small GPU-friendly helpers that Slang can inline into the
// importing shader. Prefer branch-light arithmetic and native intrinsics here; avoid global resources or
// engine-specific bindings so API users can import it safely.

// -------------------------------------------------------------------------------------------------
// Constants
// -------------------------------------------------------------------------------------------------

static const float SFT_PI = 3.14159265358979323846;
static const float SFT_HALF_PI = 1.57079632679489661923;
static const float SFT_INV_PI = 0.31830988618379067154;
static const float SFT_INV_TWO_PI = 0.15915494309189533577;
static const float SFT_EPSILON = 1.0e-6;

// -------------------------------------------------------------------------------------------------
// Small shared types
// -------------------------------------------------------------------------------------------------

struct SurfaceBasis {
    float3 normal;
    float3 tangent;
    float3 bitangent;
};



// -------------------------------------------------------------------------------------------------
// Scalar/vector math helpers
// -------------------------------------------------------------------------------------------------

float square(float v) { return v * v; }
float2 square(float2 v) { return v * v; }
float3 square(float3 v) { return v * v; }
float4 square(float4 v) { return v * v; }

float pow2(float v) { return v * v; }
float pow3(float v) { return v * v * v; }
float pow4(float v) { float v2 = v * v; return v2 * v2; }
float pow5(float v) { float v2 = v * v; return v2 * v2 * v; }

float clamp01(float v) { return saturate(v); }
float2 clamp01(float2 v) { return saturate(v); }
float3 clamp01(float3 v) { return saturate(v); }
float4 clamp01(float4 v) { return saturate(v); }

float signNotZero(float v) { return v >= 0.0 ? 1.0 : -1.0; }
float2 signNotZero(float2 v) { return float2(signNotZero(v.x), signNotZero(v.y)); }
float3 signNotZero(float3 v) { return float3(signNotZero(v.x), signNotZero(v.y), signNotZero(v.z)); }

float safeRcp(float v) {
    return signNotZero(v) / max(abs(v), SFT_EPSILON);
}

float2 safeRcp(float2 v) {
    return signNotZero(v) / max(abs(v), SFT_EPSILON);
}

float3 safeRcp(float3 v) {
    return signNotZero(v) / max(abs(v), SFT_EPSILON);
}

float4 safeRcp(float4 v) {
    float4 s = float4(signNotZero(v.x), signNotZero(v.y), signNotZero(v.z), signNotZero(v.w));
    return s / max(abs(v), SFT_EPSILON);
}

float safeDiv(float numerator, float denominator) {
    return numerator * safeRcp(denominator);
}

float remap(float value, float inputMin, float inputMax, float outputMin, float outputMax) {
    return outputMin + (value - inputMin) * (outputMax - outputMin) * safeRcp(inputMax - inputMin);
}

float remap01(float value, float inputMin, float inputMax) {
    return saturate((value - inputMin) * safeRcp(inputMax - inputMin));
}

float2 safeNormalize(float2 v) {
    return v * rsqrt(max(dot(v, v), SFT_EPSILON));
}

float3 safeNormalize(float3 v) {
    return v * rsqrt(max(dot(v, v), SFT_EPSILON));
}

float4 safeNormalize(float4 v) {
    return v * rsqrt(max(dot(v, v), SFT_EPSILON));
}

float3 safeNormalizeOr(float3 v, float3 fallback) {
    float len2 = dot(v, v);
    return len2 > SFT_EPSILON ? v * rsqrt(len2) : fallback;
}

float max3(float3 v) { return max(max(v.x, v.y), v.z); }
float min3(float3 v) { return min(min(v.x, v.y), v.z); }

// The middle of three values (not the mean) — MSDF text sampling collapses a 3-channel distance
// field to one scalar this way (msdfgen's own reference reconstruction), which is what keeps MSDF
// antialiasing free of color fringing: the median is a distance value, never a per-channel color.
float median3(float3 v) { return max(min(v.x, v.y), min(max(v.x, v.y), v.z)); }

// -------------------------------------------------------------------------------------------------
// Fullscreen triangle helpers
// -------------------------------------------------------------------------------------------------

float2 fullscreenTriangleUv(uint vertexId) {
    return float2((vertexId << 1) & 2, vertexId & 2);
}

float4 fullscreenTrianglePosition(float2 uv) {
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

// -------------------------------------------------------------------------------------------------
// Color and tonemapping
// -------------------------------------------------------------------------------------------------

float luminance(float3 linearRgb) {
    return dot(linearRgb, float3(0.2126, 0.7152, 0.0722));
}

float3 reinhardTonemap(float3 hdr) {
    return hdr / (hdr + 1.0);
}

float3 exposureTonemap(float3 hdr, float exposure) {
    // Equivalent to 1 - exp(-hdr * exposure), using exp2 for cheaper target code on many GPUs.
    return 1.0 - exp2(-hdr * exposure * 1.4426950408889634);
}

float3 acesFittedTonemap(float3 hdr) {
    // Narkowicz ACES fit: compact, branchless, and good enough for real-time previews. Not one of
    // the engine's facilitated tonemap operators (see Engine::ToneMappingOperator) — kept here only
    // as a helper for anyone who wants to wire ACES up by hand.
    float3 x = max(hdr, 0.0);
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// SMPTE ST 2084 (PQ) forward OETF. `nits` is absolute display luminance in cd/m^2; the result is
// the normalized [0,1] code value an HDR10 (RHI::ColorSpace::Hdr10St2084) swapchain expects the
// shader to have already encoded — Vulkan applies no fixed-function PQ curve the way an *Srgb
// swapchain format applies sRGB automatically, so every HDR-output tonemap operator must call this
// itself before writing its final color.
float3 pqEncode(float3 nits) {
    static const float PQ_M1 = 0.1593017578125;
    static const float PQ_M2 = 78.84375;
    static const float PQ_C1 = 0.8359375;
    static const float PQ_C2 = 18.8515625;
    static const float PQ_C3 = 18.6875;
    static const float PQ_MAX_NITS = 10000.0;

    float3 y = saturate(nits / PQ_MAX_NITS);
    float3 yM1 = pow(y, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * yM1) / (1.0 + PQ_C3 * yM1), PQ_M2);
}

float linearToSrgbChannel(float linearValue) {
    return linearValue <= 0.0031308
        ? linearValue * 12.92
        : 1.055 * pow(max(linearValue, 0.0), 1.0 / 2.4) - 0.055;
}

float3 linearToSrgb(float3 linearRgb) {
    return float3(
        linearToSrgbChannel(linearRgb.r),
        linearToSrgbChannel(linearRgb.g),
        linearToSrgbChannel(linearRgb.b));
}

float srgbToLinearChannel(float srgbValue) {
    return srgbValue <= 0.04045
        ? srgbValue / 12.92
        : pow(max((srgbValue + 0.055) / 1.055, 0.0), 2.4);
}

float3 srgbToLinear(float3 srgb) {
    return float3(
        srgbToLinearChannel(srgb.r),
        srgbToLinearChannel(srgb.g),
        srgbToLinearChannel(srgb.b));
}

// -------------------------------------------------------------------------------------------------
// Color-space structs and conversions
// -------------------------------------------------------------------------------------------------

static const float SFT_COLOR_EPSILON = 1.0e-10;
static const float SFT_DEGREES_PER_RADIAN = 57.2957795130823208768;
static const float SFT_RADIANS_PER_DEGREE = 0.01745329251994329577;

struct LinearColor { float r; float g; float b; float a; };
struct SrgbColor { float r; float g; float b; float a; };
struct XyzColor { float x; float y; float z; float alpha; };
struct AdobeRgbColor { float r; float g; float b; float a; };
struct DisplayP3Color { float r; float g; float b; float a; };
struct Rec2020Color { float r; float g; float b; float a; };
struct HslColor { float h; float s; float l; float a; };
struct HsvColor { float h; float s; float v; float a; };
struct HwbColor { float h; float w; float b; float a; };
struct LabColor { float l; float a; float b; float alpha; };
struct LchColor { float l; float c; float h; float a; };
struct LuvColor { float l; float u; float v; float alpha; };
struct OklabColor { float l; float a; float b; float alpha; };
struct OklchColor { float l; float c; float h; float alpha; };

LinearColor makeLinearColor(float r, float g, float b, float a) {
    LinearColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}

float3 linearColorRgb(LinearColor c) { return float3(c.r, c.g, c.b); }
LinearColor linearColorFromRgb(float3 rgb, float alpha) { return makeLinearColor(rgb.r, rgb.g, rgb.b, alpha); }
float colorWrapDegrees(float h) { float w = fmod(h, 360.0); return w < 0.0 ? w + 360.0 : w; }

XyzColor linearColorToXyz(LinearColor c) {
    XyzColor outColor;
    outColor.x = 0.4124 * c.r + 0.3576 * c.g + 0.1805 * c.b;
    outColor.y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    outColor.z = 0.0193 * c.r + 0.1192 * c.g + 0.9505 * c.b;
    outColor.alpha = c.a;
    return outColor;
}

LinearColor xyzToLinearColor(XyzColor c) {
    return makeLinearColor(
        3.2406 * c.x - 1.5372 * c.y - 0.4986 * c.z,
        -0.9689 * c.x + 1.8758 * c.y + 0.0415 * c.z,
        0.0557 * c.x - 0.2040 * c.y + 1.0570 * c.z,
        c.alpha);
}

SrgbColor linearColorToSrgbColor(LinearColor c) {
    SrgbColor outColor;
    outColor.r = linearToSrgbChannel(c.r);
    outColor.g = linearToSrgbChannel(c.g);
    outColor.b = linearToSrgbChannel(c.b);
    outColor.a = c.a;
    return outColor;
}

LinearColor srgbColorToLinearColor(SrgbColor c) {
    return makeLinearColor(srgbToLinearChannel(c.r), srgbToLinearChannel(c.g), srgbToLinearChannel(c.b), c.a);
}

AdobeRgbColor linearColorToAdobeRgb(LinearColor c) {
    XyzColor xyz = linearColorToXyz(c);
    float rl = 2.04159 * xyz.x - 0.56501 * xyz.y - 0.34473 * xyz.z;
    float gl = -0.96924 * xyz.x + 1.87597 * xyz.y + 0.04156 * xyz.z;
    float bl = 0.01344 * xyz.x - 0.11836 * xyz.y + 1.01517 * xyz.z;
    AdobeRgbColor outColor;
    outColor.r = abs(rl) < SFT_COLOR_EPSILON ? 0.0 : pow(rl, 256.0 / 563.0);
    outColor.g = abs(gl) < SFT_COLOR_EPSILON ? 0.0 : pow(gl, 256.0 / 563.0);
    outColor.b = abs(bl) < SFT_COLOR_EPSILON ? 0.0 : pow(bl, 256.0 / 563.0);
    outColor.a = c.a;
    return outColor;
}

LinearColor adobeRgbToLinearColor(AdobeRgbColor c) {
    float rl = abs(c.r) < SFT_COLOR_EPSILON ? 0.0 : pow(c.r, 563.0 / 256.0);
    float gl = abs(c.g) < SFT_COLOR_EPSILON ? 0.0 : pow(c.g, 563.0 / 256.0);
    float bl = abs(c.b) < SFT_COLOR_EPSILON ? 0.0 : pow(c.b, 563.0 / 256.0);
    XyzColor xyz;
    xyz.x = 0.57667 * rl + 0.18556 * gl + 0.18823 * bl;
    xyz.y = 0.29734 * rl + 0.62736 * gl + 0.07529 * bl;
    xyz.z = 0.02703 * rl + 0.07069 * gl + 0.99134 * bl;
    xyz.alpha = c.a;
    return xyzToLinearColor(xyz);
}

DisplayP3Color linearColorToDisplayP3(LinearColor c) {
    XyzColor xyz = linearColorToXyz(c);
    float rl = 1.2249 * xyz.x - 0.2247 * xyz.y - 0.0040 * xyz.z;
    float gl = -0.0420 * xyz.x + 1.0419 * xyz.y + 0.0001 * xyz.z;
    float bl = -0.0776 * xyz.y + 0.9398 * xyz.z;
    DisplayP3Color outColor;
    outColor.r = linearToSrgbChannel(rl);
    outColor.g = linearToSrgbChannel(gl);
    outColor.b = linearToSrgbChannel(bl);
    outColor.a = c.a;
    return outColor;
}

LinearColor displayP3ToLinearColor(DisplayP3Color c) {
    float rl = srgbToLinearChannel(c.r);
    float gl = srgbToLinearChannel(c.g);
    float bl = srgbToLinearChannel(c.b);
    XyzColor xyz;
    xyz.x = 0.486569 * rl + 0.265673 * gl + 0.198187 * bl;
    xyz.y = 0.228973 * rl + 0.691752 * gl + 0.0792749 * bl;
    xyz.z = 0.0451143 * gl + 1.04379 * bl;
    xyz.alpha = c.a;
    return xyzToLinearColor(xyz);
}

Rec2020Color linearColorToRec2020(LinearColor c) {
    XyzColor xyz = linearColorToXyz(c);
    float rl = 1.7166634 * xyz.x - 0.3556733 * xyz.y - 0.2533681 * xyz.z;
    float gl = -0.6666738 * xyz.x + 1.6164557 * xyz.y + 0.0157683 * xyz.z;
    float bl = 0.0176425 * xyz.x - 0.0427769 * xyz.y + 0.9422433 * xyz.z;
    Rec2020Color outColor;
    outColor.r = pow(saturate(rl), 1.0 / 2.4);
    outColor.g = pow(saturate(gl), 1.0 / 2.4);
    outColor.b = pow(saturate(bl), 1.0 / 2.4);
    outColor.a = c.a;
    return outColor;
}

LinearColor rec2020ToLinearColor(Rec2020Color c) {
    float rl = pow(saturate(c.r), 2.4);
    float gl = pow(saturate(c.g), 2.4);
    float bl = pow(saturate(c.b), 2.4);
    float x = 0.636958 * rl + 0.144617 * gl + 0.168881 * bl;
    float y = 0.2627 * rl + 0.678 * gl + 0.0593 * bl;
    float z = 0.028073 * gl + 1.060985 * bl;
    return makeLinearColor(3.240969 * x - 1.537383 * y - 0.498611 * z,
                           -0.969244 * x + 1.875968 * y + 0.041555 * z,
                           0.05563 * x - 0.203977 * y + 1.056972 * z,
                           c.a);
}

float hslHueToRgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

LinearColor hslToLinearColor(HslColor c) {
    float h = c.h / 360.0;
    float q = c.l < 0.5 ? c.l * (1.0 + c.s) : c.l + c.s - c.l * c.s;
    float p = 2.0 * c.l - q;
    return makeLinearColor(hslHueToRgb(p, q, h + 1.0 / 3.0), hslHueToRgb(p, q, h), hslHueToRgb(p, q, h - 1.0 / 3.0), c.a);
}

HslColor linearColorToHsl(LinearColor c) {
    float maxc = max(max(c.r, c.g), c.b);
    float minc = min(min(c.r, c.g), c.b);
    float l = (maxc + minc) * 0.5;
    float d = maxc - minc;
    HslColor outColor; outColor.h = 0.0; outColor.s = 0.0; outColor.l = l; outColor.a = c.a;
    if (maxc != minc && abs(d) >= SFT_COLOR_EPSILON) {
        outColor.s = l > 0.5 ? d / (2.0 - maxc - minc) : d / (maxc + minc);
        float h = maxc == c.r ? ((c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0)) / 6.0
                : maxc == c.g ? ((c.b - c.r) / d + 2.0) / 6.0
                : ((c.r - c.g) / d + 4.0) / 6.0;
        outColor.h = h * 360.0;
    }
    return outColor;
}

LinearColor hsvToLinearColor(HsvColor c) {
    float h = c.h / 60.0;
    int i = int(floor(h));
    float f = h - float(i);
    float p = c.v * (1.0 - c.s);
    float q = c.v * (1.0 - c.s * f);
    float t = c.v * (1.0 - c.s * (1.0 - f));
    int sector = ((i % 6) + 6) % 6;
    if (sector == 0) return makeLinearColor(c.v, t, p, c.a);
    if (sector == 1) return makeLinearColor(q, c.v, p, c.a);
    if (sector == 2) return makeLinearColor(p, c.v, t, c.a);
    if (sector == 3) return makeLinearColor(p, q, c.v, c.a);
    if (sector == 4) return makeLinearColor(t, p, c.v, c.a);
    return makeLinearColor(c.v, p, q, c.a);
}

HsvColor linearColorToHsv(LinearColor c) {
    float maxc = max(max(c.r, c.g), c.b);
    float minc = min(min(c.r, c.g), c.b);
    float d = maxc - minc;
    HsvColor outColor; outColor.h = 0.0; outColor.s = maxc != 0.0 ? d / maxc : 0.0; outColor.v = maxc; outColor.a = c.a;
    if (d != 0.0) {
        float h = maxc == c.r ? fmod((c.g - c.b) / d, 6.0)
                : maxc == c.g ? ((c.b - c.r) / d) + 2.0
                : ((c.r - c.g) / d) + 4.0;
        outColor.h = h * 60.0;
        if (outColor.h < 0.0) outColor.h += 360.0;
    }
    return outColor;
}

LinearColor hwbToLinearColor(HwbColor c) {
    float w = saturate(c.w);
    float bl = saturate(c.b);
    float sum = w + bl;
    if (sum >= 1.0) {
        float gray = w / max(abs(sum), SFT_COLOR_EPSILON);
        return makeLinearColor(gray, gray, gray, c.a);
    }
    float v = 1.0 - bl;
    float s = v > SFT_COLOR_EPSILON ? 1.0 - w / v : 0.0;
    HsvColor hsv; hsv.h = c.h; hsv.s = s; hsv.v = v; hsv.a = c.a;
    return hsvToLinearColor(hsv);
}

HwbColor linearColorToHwb(LinearColor c) {
    HsvColor hsv = linearColorToHsv(c);
    HwbColor outColor; outColor.h = hsv.h; outColor.w = min(min(c.r, c.g), c.b); outColor.b = 1.0 - max(max(c.r, c.g), c.b); outColor.a = c.a; return outColor;
}

float labF(float t) {
    float eps = pow(6.0 / 29.0, 3.0);
    float k = (1.0 / 3.0) * pow(29.0 / 6.0, 2.0);
    return t > eps ? (abs(t) < SFT_COLOR_EPSILON ? 0.0 : pow(t, 1.0 / 3.0)) : k * t + 4.0 / 29.0;
}

float labFInv(float u) {
    float eps = 6.0 / 29.0;
    float k = 3.0 * pow(6.0 / 29.0, 2.0);
    return u > eps ? (abs(u) < SFT_COLOR_EPSILON ? 0.0 : u * u * u) : k * (u - 4.0 / 29.0);
}

LabColor linearColorToLab(LinearColor c) {
    XyzColor xyz = linearColorToXyz(c);
    float fx = labF(xyz.x / 0.95047);
    float fy = labF(xyz.y);
    float fz = labF(xyz.z / 1.08883);
    LabColor outColor; outColor.l = 116.0 * fy - 16.0; outColor.a = 500.0 * (fx - fy); outColor.b = 200.0 * (fy - fz); outColor.alpha = c.a; return outColor;
}

LinearColor labToLinearColor(LabColor c) {
    float fy = (c.l + 16.0) / 116.0;
    XyzColor xyz; xyz.x = 0.95047 * labFInv(fy + c.a / 500.0); xyz.y = labFInv(fy); xyz.z = 1.08883 * labFInv(fy - c.b / 200.0); xyz.alpha = c.alpha; return xyzToLinearColor(xyz);
}

LchColor linearColorToLch(LinearColor c) {
    LabColor lab = linearColorToLab(c);
    float chroma = sqrt(lab.a * lab.a + lab.b * lab.b);
    LchColor outColor; outColor.l = lab.l; outColor.c = chroma; outColor.h = abs(chroma) < SFT_COLOR_EPSILON ? 0.0 : colorWrapDegrees(atan2(lab.b, lab.a) * SFT_DEGREES_PER_RADIAN); outColor.a = lab.alpha; return outColor;
}

LinearColor lchToLinearColor(LchColor c) {
    float chroma = abs(c.c) < SFT_COLOR_EPSILON ? 0.0 : c.c;
    float h = c.h * SFT_RADIANS_PER_DEGREE;
    LabColor lab; lab.l = c.l; lab.a = chroma * cos(h); lab.b = chroma * sin(h); lab.alpha = c.a; return labToLinearColor(lab);
}

float luvUPrime(float x, float y, float z) { float d = x + 15.0 * y + 3.0 * z; return abs(d) < SFT_COLOR_EPSILON ? 0.0 : 4.0 * x / d; }
float luvVPrime(float x, float y, float z) { float d = x + 15.0 * y + 3.0 * z; return abs(d) < SFT_COLOR_EPSILON ? 0.0 : 9.0 * y / d; }

LuvColor linearColorToLuv(LinearColor c) {
    XyzColor xyz = linearColorToXyz(c);
    float l = xyz.y > 0.008856 ? 116.0 * pow(xyz.y, 1.0 / 3.0) - 16.0 : 903.3 * xyz.y;
    float un = luvUPrime(0.95047, 1.0, 1.08883);
    float vn = luvVPrime(0.95047, 1.0, 1.08883);
    LuvColor outColor; outColor.l = l; outColor.u = 13.0 * l * (luvUPrime(xyz.x, xyz.y, xyz.z) - un); outColor.v = 13.0 * l * (luvVPrime(xyz.x, xyz.y, xyz.z) - vn); outColor.alpha = c.a; return outColor;
}

LinearColor luvToLinearColor(LuvColor c) {
    float upRef = luvUPrime(0.95047, 1.0, 1.08883);
    float vpRef = luvVPrime(0.95047, 1.0, 1.08883);
    float yr = c.l > 8.0 ? pow((c.l + 16.0) / 116.0, 3.0) : c.l / 903.3;
    float up = abs(c.l) < SFT_COLOR_EPSILON ? upRef : c.u / (13.0 * c.l) + upRef;
    float vp = abs(c.l) < SFT_COLOR_EPSILON ? vpRef : c.v / (13.0 * c.l) + vpRef;
    float d = max(abs(4.0 * vp), SFT_COLOR_EPSILON);
    XyzColor xyz; xyz.x = yr * 9.0 * up / d; xyz.y = yr; xyz.z = yr * (12.0 - 3.0 * up - 20.0 * vp) / d; xyz.alpha = c.alpha; return xyzToLinearColor(xyz);
}

OklabColor linearColorToOklab(LinearColor c) {
    float3 rgb = saturate(linearColorRgb(c));
    float l = 0.4122214708 * rgb.r + 0.5363325363 * rgb.g + 0.0514459929 * rgb.b;
    float m = 0.2119034982 * rgb.r + 0.6806995451 * rgb.g + 0.1073969566 * rgb.b;
    float s = 0.0883024619 * rgb.r + 0.2817188376 * rgb.g + 0.6299787005 * rgb.b;
    float ll = abs(l) < SFT_COLOR_EPSILON ? 0.0 : pow(l, 1.0 / 3.0);
    float mm = abs(m) < SFT_COLOR_EPSILON ? 0.0 : pow(m, 1.0 / 3.0);
    float ss = abs(s) < SFT_COLOR_EPSILON ? 0.0 : pow(s, 1.0 / 3.0);
    OklabColor outColor; outColor.l = saturate(0.2104542553 * ll + 0.7936177850 * mm - 0.0040720468 * ss); outColor.a = clamp(1.9779984951 * ll - 2.4285922050 * mm + 0.4505937099 * ss, -0.5, 0.5); outColor.b = clamp(0.0259040371 * ll + 0.7827717662 * mm - 0.8086757660 * ss, -0.5, 0.5); outColor.alpha = saturate(c.a); return outColor;
}

LinearColor oklabToLinearColor(OklabColor c) {
    float l = c.l + 0.3963377774 * c.a + 0.2158037573 * c.b;
    float m = c.l - 0.1055613458 * c.a - 0.0638541728 * c.b;
    float s = c.l - 0.0894841775 * c.a - 1.2914855480 * c.b;
    float l3 = abs(l) < SFT_COLOR_EPSILON ? 0.0 : l * l * l;
    float m3 = abs(m) < SFT_COLOR_EPSILON ? 0.0 : m * m * m;
    float s3 = abs(s) < SFT_COLOR_EPSILON ? 0.0 : s * s * s;
    return makeLinearColor(saturate(4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3), saturate(-1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3), saturate(0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3), saturate(c.alpha));
}

OklchColor linearColorToOklch(LinearColor c) {
    OklabColor lab = linearColorToOklab(c);
    float chroma = sqrt(lab.a * lab.a + lab.b * lab.b);
    OklchColor outColor; outColor.l = lab.l; outColor.c = chroma; outColor.h = abs(chroma) < SFT_COLOR_EPSILON ? 0.0 : colorWrapDegrees(atan2(lab.b, lab.a) * SFT_DEGREES_PER_RADIAN); outColor.alpha = lab.alpha; return outColor;
}

LinearColor oklchToLinearColor(OklchColor c) {
    float chroma = abs(c.c) < SFT_COLOR_EPSILON ? 0.0 : c.c;
    float h = c.h * SFT_RADIANS_PER_DEGREE;
    OklabColor lab; lab.l = c.l; lab.a = chroma * cos(h); lab.b = chroma * sin(h); lab.alpha = c.alpha; return oklabToLinearColor(lab);
}

// -------------------------------------------------------------------------------------------------
// Frustum culling
// -------------------------------------------------------------------------------------------------

// Six inward-facing clip planes (ax + by + cz + d >= 0 for a point inside), packed one per row —
// row i is (a, b, c, d). GPU-side counterpart of Renderer::Frustum/frustum_from_view_projection
// (Culling.hpp) — same Gribb/Hartmann extraction, same left/right/bottom/top/near/far order, kept
// numerically identical so a GPU cull pass and its CPU fallback never disagree on borderline cases.
float4x4 extractFrustumPlanesLeftRightBottomTop(float4x4 viewProjection) {
    float4 r0 = float4(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
    float4 r1 = float4(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
    float4 r3 = float4(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);
    float4 left = r3 + r0;
    float4 right = r3 - r0;
    float4 bottom = r3 + r1;
    float4 top = r3 - r1;
    return float4x4(left, right, bottom, top);
}

float4x4 extractFrustumPlanesNearFar(float4x4 viewProjection) {
    float4 r2 = float4(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
    float4 r3 = float4(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);
    float4 near = r3 + r2;
    float4 far = r3 - r2;
    return float4x4(near, far, float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0));
}

bool frustumPlaneIntersectsSphere(float4 plane, float3 center, float radius) {
    float3 normal = plane.xyz;
    float invLength = rsqrt(max(dot(normal, normal), SFT_EPSILON));
    return dot(normal * invLength, center) + plane.w * invLength >= -radius;
}

// Conservative sphere-vs-frustum test, matching Renderer::frustum_intersects_sphere's contract
// exactly (false only when the sphere is fully outside at least one plane).
bool frustumIntersectsSphere(float4x4 viewProjection, float3 center, float radius) {
    float4x4 leftRightBottomTop = extractFrustumPlanesLeftRightBottomTop(viewProjection);
    float4x4 nearFar = extractFrustumPlanesNearFar(viewProjection);
    if (!frustumPlaneIntersectsSphere(leftRightBottomTop[0], center, radius)) return false;
    if (!frustumPlaneIntersectsSphere(leftRightBottomTop[1], center, radius)) return false;
    if (!frustumPlaneIntersectsSphere(leftRightBottomTop[2], center, radius)) return false;
    if (!frustumPlaneIntersectsSphere(leftRightBottomTop[3], center, radius)) return false;
    if (!frustumPlaneIntersectsSphere(nearFar[0], center, radius)) return false;
    if (!frustumPlaneIntersectsSphere(nearFar[1], center, radius)) return false;
    return true;
}

// -------------------------------------------------------------------------------------------------
// Normal packing and geometric helpers
// -------------------------------------------------------------------------------------------------

float3 encodeUnitVectorUnorm(float3 unitVector) {
    return unitVector * 0.5 + 0.5;
}

float3 decodeUnitVectorUnorm(float3 encoded) {
    return encoded * 2.0 - 1.0;
}

float2 encodeOctahedralNormal(float3 normal) {
    normal *= safeRcp(abs(normal.x) + abs(normal.y) + abs(normal.z));
    float2 wrapped = (1.0 - abs(normal.yx)) * signNotZero(normal.xy);
    float2 oct = normal.z >= 0.0 ? normal.xy : wrapped;
    return oct * 0.5 + 0.5;
}

float3 decodeOctahedralNormal(float2 encoded) {
    float2 f = encoded * 2.0 - 1.0;
    float3 normal = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -t : t;
    normal.y += normal.y >= 0.0 ? -t : t;
    return safeNormalize(normal);
}

SurfaceBasis makeSurfaceBasis(float3 normal) {
    float3 n = safeNormalize(normal);
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;

    SurfaceBasis basis;
    basis.normal = n;
    basis.tangent = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    basis.bitangent = float3(b, s + n.y * n.y * a, -n.y);
    return basis;
}

// Dielectric F0 (specular reflectance at normal incidence) from index of refraction. It returns a
// scalar because a bare-dielectric F0 is achromatic; glTF's default ior=1.5 reproduces the fixed
// 0.04 constant most PBR material encodings use.
float dielectricF0FromIor(float ior) {
    float f = (ior - 1.0) * safeRcp(ior + 1.0);
    return f * f;
}
