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
static const float SFT_TWO_PI = 6.28318530717958647692;
static const float SFT_HALF_PI = 1.57079632679489661923;
static const float SFT_INV_PI = 0.31830988618379067154;
static const float SFT_INV_TWO_PI = 0.15915494309189533577;
static const float SFT_EPSILON = 1.0e-6;
static const float SFT_MIN_ROUGHNESS = 0.045;

// -------------------------------------------------------------------------------------------------
// Small shared types
// -------------------------------------------------------------------------------------------------

struct SurfaceBasis {
    float3 normal;
    float3 tangent;
    float3 bitangent;
};

struct PbrMaterial {
    float3 albedo;
    float roughness;
    float metallic;
    float ambientOcclusion;
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
    // Narkowicz ACES fit: compact, branchless, and good enough for real-time previews.
    float3 x = max(hdr, 0.0);
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
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

// -------------------------------------------------------------------------------------------------
// PBR building blocks
// -------------------------------------------------------------------------------------------------

float perceptualRoughnessToRoughness(float perceptualRoughness) {
    return max(square(perceptualRoughness), SFT_MIN_ROUGHNESS);
}

float roughnessToPerceptualRoughness(float roughness) {
    return sqrt(max(roughness, 0.0));
}

float diffuseWrap(float nDotL, float wrap) {
    return saturate((nDotL + wrap) * safeRcp(1.0 + wrap));
}

float3 fresnelSchlick(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow5(1.0 - saturate(cosTheta));
}

float3 fresnelSchlickRoughness(float cosTheta, float3 f0, float roughness) {
    return f0 + (max(float3(1.0 - roughness), f0) - f0) * pow5(1.0 - saturate(cosTheta));
}

float distributionGGX(float nDotH, float roughness) {
    float a = max(roughness, SFT_MIN_ROUGHNESS);
    float a2 = a * a;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 * SFT_INV_PI * safeRcp(d * d);
}

float geometrySchlickGGX(float nDotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return nDotV * safeRcp(nDotV * (1.0 - k) + k);
}

float geometrySmithGGX(float nDotV, float nDotL, float roughness) {
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

float3 lambertDiffuse(float3 albedo) {
    return albedo * SFT_INV_PI;
}

float3 dielectricF0(float reflectance) {
    return float3(square((reflectance - 1.0) / (reflectance + 1.0)));
}

float3 pbrF0(float3 albedo, float metallic, float dielectricReflectance) {
    return lerp(dielectricF0(dielectricReflectance), albedo, saturate(metallic));
}

// -------------------------------------------------------------------------------------------------
// Low-cost deterministic hashes and noise seeds
// -------------------------------------------------------------------------------------------------

uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

uint hashUint2(uint2 v) {
    return hashUint(v.x ^ hashUint(v.y));
}

uint hashUint3(uint3 v) {
    return hashUint(v.x ^ hashUint(v.y) ^ hashUint(v.z));
}

float hashToUnitFloat(uint x) {
    return float(hashUint(x) & 0x00ffffffu) * (1.0 / 16777216.0);
}

float hash11(float p) {
    return hashToUnitFloat(asuint(p));
}

float hash12(float2 p) {
    return hashToUnitFloat(hashUint2(asuint(p)));
}

float hash13(float3 p) {
    return hashToUnitFloat(hashUint3(asuint(p)));
}
