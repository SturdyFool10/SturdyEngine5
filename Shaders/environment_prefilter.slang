// Prefilters the sky (the atmosphere's sky-view LUT) into the roughness-level atlas of sturdy_specular.slang.
//
// One thread per atlas texel; texels of levels not selected by `levelMask` return at once, so the engine can refresh one
// level per frame (the sky changes slowly) at a fraction of the cost of the whole atlas. Each rough texel importance-samples
// the GGX lobe around its direction with a fixed per-texel rotation, so the result is stable from frame to frame. The sun
// disk is not in the sky-view LUT: its specular highlight comes from the direct sun term, never twice.
import sturdy_common;
import sturdy_atmosphere;
import sturdy_specular;

static const uint2 SFT_SKY_VIEW_LUT_SIZE = uint2(192, 108);

struct PrefilterConstants {
    uint levelMask;
    uint sampleCount;
    uint2 padding;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<PrefilterConstants> prefilter;
#else
[[push_constant]] ConstantBuffer<PrefilterConstants> prefilter;
#endif

ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float4> skyViewLut;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> environmentOut;

float3 skyRadiance(float3 direction) {
    AtmosphereParams atmosphere = unpackAtmosphereParams(atmosphereData);
    float3 planetOrigin = atmosphereData.cameraPositionPlanetSpace.xyz;
    float3 sunDirToward = safeNormalizeOr(atmosphereData.sunDirectionAngularRadius.xyz, float3(0.0, 1.0, 0.0));
    float viewHeight = max(length(planetOrigin), atmosphere.planetRadiusMeters + 1.0);
    float tGroundUnused;
    bool hitsGround = distanceToGround(atmosphere, planetOrigin, direction, tGroundUnused);
    float2 uv = skyViewLutUv(atmosphere, direction, float3(0.0, 1.0, 0.0), sunDirToward, viewHeight, hitsGround, SFT_SKY_VIEW_LUT_SIZE);
    return skyViewLut.SampleLevel(linearSampler, uv, 0.0).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= SFT_ENV_ATLAS_SIZE)) {
        return;
    }
    // Which level owns this texel.
    uint level = SFT_ENV_LEVELS;
    uint2 origin = uint2(0, 0);
    uint size = 0;
    for (uint candidate = 0; candidate < SFT_ENV_LEVELS; ++candidate) {
        uint2 candidateOrigin;
        uint candidateSize;
        environmentLevelRect(candidate, candidateOrigin, candidateSize);
        if (all(id.xy >= candidateOrigin) && all(id.xy < candidateOrigin + candidateSize)) {
            level = candidate;
            origin = candidateOrigin;
            size = candidateSize;
        }
    }
    if (level >= SFT_ENV_LEVELS || (prefilter.levelMask & (1u << level)) == 0u) {
        return;
    }

    float3 normal = decodeOctahedralNormal((float2(id.xy - origin) + 0.5) / float(size));
    float perceptualRoughness = float(level) / float(SFT_ENV_LEVELS - 1);
    float alphaRoughness = max(perceptualRoughness * perceptualRoughness, 1.0e-3);
    if (level == 0u) {
        environmentOut.Store(int2(id.xy), float4(skyRadiance(normal), 1.0));
        return;
    }

    SurfaceBasis basis = makeSurfaceBasis(normal);
    uint count = max(prefilter.sampleCount, 1u);
    float rotation = float(pcgHash(id.x + pcgHash(id.y)) & 0xffffu) / 65536.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [loop]
    for (uint i = 0; i < count; ++i) {
        float2 u = hammersley(i, count);
        u.x = frac(u.x + rotation);
        float3 h = sampleGgxNormal(u, alphaRoughness);
        float3 halfVector = basis.tangent * h.x + basis.bitangent * h.y + basis.normal * h.z;
        // Split-sum assumption: view = normal, so the light direction is the view reflected about the microfacet.
        float3 lightVector = 2.0 * dot(normal, halfVector) * halfVector - normal;
        float nDotL = dot(normal, lightVector);
        if (nDotL > 0.0) {
            sum += skyRadiance(lightVector) * nDotL;
            weight += nDotL;
        }
    }
    environmentOut.Store(int2(id.xy), float4(sum / max(weight, 1.0e-4), 1.0));
}
