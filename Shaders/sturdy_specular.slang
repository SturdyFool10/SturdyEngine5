// Indirect specular building blocks shared by the deferred lighting pass, the environment prefilter and the screen-space
// reflection passes: the prefiltered-environment atlas layout, the split-sum DFG with multiple-scattering compensation,
// specular occlusion, bounded-VNDF GGX sampling, and the sample sequences they use.
import sturdy_common;
import sturdy_pbr;

// ---- prefiltered environment atlas -----------------------------------------------------------------------------------
//
// One octahedral map per roughness level, perceptual roughness level / (LEVELS - 1): 128^2 for the mirror level, halving
// per level. Level 0 sits at the left; levels 1.. are stacked in a column to its right (192 x 128 in total). A 2D atlas
// rather than a cube or a mip chain so every backend binds it as a plain sampled/storage texture, and one dispatch can
// refresh any subset of levels. Mirrors `kEnvironmentAtlas*` in Renderer/Reflections.cpp.

static const uint SFT_ENV_LEVELS = 6;
static const uint SFT_ENV_BASE_SIZE = 128;
static const uint2 SFT_ENV_ATLAS_SIZE = uint2(192, 128);

void environmentLevelRect(uint level, out uint2 origin, out uint size) {
    size = SFT_ENV_BASE_SIZE >> level;
    origin = uint2(0, 0);
    if (level > 0) {
        uint y = 0;
        for (uint k = 1; k < level; ++k) {
            y += SFT_ENV_BASE_SIZE >> k;
        }
        origin = uint2(SFT_ENV_BASE_SIZE, y);
    }
}

float3 sampleEnvironmentLevel(Texture2D<float4> atlas, SamplerState linearSampler, float3 direction, uint level) {
    uint2 origin;
    uint size;
    environmentLevelRect(level, origin, size);
    // Half a texel in from the edge so the bilinear footprint never reaches the neighbouring level.
    float2 texel = clamp(encodeOctahedralNormal(direction) * float(size), float2(0.5, 0.5), float2(float(size) - 0.5, float(size) - 0.5));
    return atlas.SampleLevel(linearSampler, (float2(origin) + texel) / float2(SFT_ENV_ATLAS_SIZE), 0.0).rgb;
}

// Radiance reflected towards a direction by a surface of the given perceptual roughness (split-sum: the lobe is assumed
// to be centred on `direction` and seen head-on).
float3 sampleEnvironment(Texture2D<float4> atlas, SamplerState linearSampler, float3 direction, float perceptualRoughness) {
    float level = saturate(perceptualRoughness) * float(SFT_ENV_LEVELS - 1);
    uint lower = min((uint)floor(level), SFT_ENV_LEVELS - 1);
    uint upper = min(lower + 1, SFT_ENV_LEVELS - 1);
    return lerp(sampleEnvironmentLevel(atlas, linearSampler, direction, lower),
                sampleEnvironmentLevel(atlas, linearSampler, direction, upper), frac(level));
}

// ---- split-sum BRDF ---------------------------------------------------------------------------------------------------

// Analytic fit of the GGX directional albedo split into F0 scale and bias (Karis, "Physically Based Shading on Mobile",
// 2014): no LUT texture to bind, and accurate to a few percent, which the multiple-scattering term below dominates anyway.
float2 environmentBrdfApprox(float nDotV, float perceptualRoughness) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = perceptualRoughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nDotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Fraction of incident light a surface reflects specularly, including the energy single-scattering GGX loses on rough
// surfaces (Fdez-Agüera, "A Multiple-Scattering Microfacet Model for Real-Time Image Based Lighting", 2019). Without the
// second term rough metals come out visibly too dark.
float3 specularReflectance(float3 f0, float2 dfg) {
    float3 singleScatter = f0 * dfg.x + dfg.y;
    float singleEnergy = dfg.x + dfg.y;
    float multiEnergy = 1.0 - singleEnergy;
    float3 averageFresnel = f0 + (1.0 - f0) / 21.0;
    float3 multiScatter = multiEnergy * singleScatter * averageFresnel / max(1.0 - multiEnergy * averageFresnel, 1.0e-4);
    return singleScatter + multiScatter;
}

// Rough lobes lean towards the normal (Lagarde & de Rousiers, "Moving Frostbite to PBR", 2014).
float3 specularDominantDirection(float3 normal, float3 reflected, float alphaRoughness) {
    float smoothness = saturate(1.0 - alphaRoughness);
    float factor = smoothness * (sqrt(smoothness) + alphaRoughness);
    return safeNormalizeOr(lerp(normal, reflected, factor), reflected);
}

// Specular visibility from diffuse ambient occlusion (Lagarde & de Rousiers 2014): keeps enclosed spaces from reflecting
// the sky through their walls.
float specularOcclusion(float nDotV, float ambientOcclusion, float alphaRoughness) {
    return saturate(pow(max(nDotV + ambientOcclusion, 0.0), exp2(-16.0 * alphaRoughness - 1.0)) - 1.0 + ambientOcclusion);
}

// ---- sampling ---------------------------------------------------------------------------------------------------------

uint pcgHash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Per-pixel random offset (stable over time) rotated by an R2 low-discrepancy step per frame (Cranley-Patterson rotation):
// each pixel walks a well-stratified sequence, so temporal accumulation converges much faster than with white noise.
float2 temporalSample2(uint2 pixel, uint frame) {
    uint h = pcgHash(pixel.x + pcgHash(pixel.y));
    float2 offset = float2(float(h & 0xffffu), float(pcgHash(h) & 0xffffu)) / 65536.0;
    float n = float(frame % 1024u);
    float2 r2 = frac(n * float2(0.7548776662, 0.5698402910));
    return frac(offset + r2);
}

float2 hammersley(uint index, uint count) {
    return float2(float(index) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

// GGX microfacet normal around +Z (tangent space), sampled proportionally to D * cos.
float3 sampleGgxNormal(float2 u, float alphaRoughness) {
    float a2 = alphaRoughness * alphaRoughness;
    float phi = 2.0 * SFT_PI * u.x;
    float cosTheta = sqrt(saturate((1.0 - u.y) / (1.0 + (a2 - 1.0) * u.y)));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Visible-normal GGX sampling restricted to the spherical cap that can actually reflect above the surface (Eto & Tokuyoshi,
// "Bounded VNDF Sampling for Smith-GGX Reflections", 2023). `wi` is the view direction in tangent space (+Z = normal).
// Wastes far fewer samples below the horizon than plain VNDF sampling, which matters at one ray per pixel.
float3 sampleGgxBoundedVndf(float3 wi, float alphaRoughness, float2 u) {
    float3 wiStd = safeNormalizeOr(float3(wi.xy * alphaRoughness, wi.z), float3(0.0, 0.0, 1.0));
    float phi = 2.0 * SFT_PI * u.x;
    float a = saturate(alphaRoughness);
    float s = 1.0 + length(wi.xy);
    float a2 = a * a;
    float s2 = s * s;
    float k = (1.0 - a2) * s2 / (s2 + a2 * wi.z * wi.z);
    float b = wi.z > 0.0 ? k * wiStd.z : wiStd.z;
    float z = (1.0 - u.y) * (1.0 + b) - b;
    float sinTheta = sqrt(saturate(1.0 - z * z));
    float3 oStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);
    float3 mStd = wiStd + oStd;
    return safeNormalizeOr(float3(mStd.xy * alphaRoughness, mStd.z), float3(0.0, 0.0, 1.0));
}

// Density of the *reflected* direction produced by `sampleGgxBoundedVndf` through microfacet normal `m`.
float ggxBoundedVndfReflectionPdf(float3 wi, float3 m, float alphaRoughness) {
    float ndf = distributionGGXLocal(saturate(m.z), alphaRoughness);
    float2 ai = alphaRoughness * wi.xy;
    float len2 = dot(ai, ai);
    float t = sqrt(len2 + wi.z * wi.z);
    if (wi.z >= 0.0) {
        float a = saturate(alphaRoughness);
        float s = 1.0 + length(wi.xy);
        float a2 = a * a;
        float s2 = s * s;
        float k = (1.0 - a2) * s2 / (s2 + a2 * wi.z * wi.z);
        return ndf / max(2.0 * (k * wi.z + t), 1.0e-6);
    }
    return ndf * (t - wi.z) / max(2.0 * len2, 1.0e-6);
}
