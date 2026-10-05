// Shared by volumetric_fog_{inject,integrate,apply}.slang. `FogConstants` mirrors `FogConstants` in
// Renderer/VolumetricFog.cpp; change both sides together.
//
// The froxel volume (screen tiles x exponentially distributed depth slices) is stored as a 2D atlas rather than a 3D
// texture: slices are laid out left-to-right, top-to-bottom, `grid.w` per row. That keeps every backend on plain 2D
// storage/sampled textures (WebGPU bind-group layouts here carry no view dimension) at the cost of doing the depth
// interpolation by hand.
import sturdy_common;
import sturdy_lighting_data;

struct FogConstants {
    float4x4 previousViewProjection;
    // tiles x, tiles y, slices, atlas columns
    uint4 grid;
    // near, max distance, slice distribution exponent, temporal blend (weight of the new frame)
    float4 depthParams;
    // density, height falloff, base height, anisotropy (Henyey-Greenstein g)
    float4 medium;
    // albedo rgb, noise strength
    float4 albedoNoise;
    // emissive rgb (radiance per unit density), noise feature size in world units
    float4 emissiveNoiseScale;
    // wind xyz (world units per second), time in seconds
    float4 windTime;
    // sun, ambient, local light scale, exposure
    float4 lightScales;
    // sun shadows, local-light shadows, history valid, frame index
    uint4 flags;
    // render width, render height, tile size in pixels, 1 = clustered local lights are bound
    float4 render;
};

// View depth at a continuous slice coordinate in [0, slices].
float fogSliceDepth(FogConstants fog, float slice) {
    float t = saturate(slice / float(max(fog.grid.z, 1u)));
    return fog.depthParams.x + (fog.depthParams.y - fog.depthParams.x) * pow(t, max(fog.depthParams.z, 1.0));
}

// Inverse of `fogSliceDepth`.
float fogSliceCoord(FogConstants fog, float viewDepth) {
    float t = saturate((viewDepth - fog.depthParams.x) / max(fog.depthParams.y - fog.depthParams.x, 1.0e-4));
    return pow(t, 1.0 / max(fog.depthParams.z, 1.0)) * float(fog.grid.z);
}

uint2 fogAtlasSize(FogConstants fog) {
    uint columns = max(fog.grid.w, 1u);
    uint rows = (fog.grid.z + columns - 1u) / columns;
    return uint2(fog.grid.x * columns, fog.grid.y * rows);
}

// Atlas texel of froxel (x, y, slice).
int2 fogAtlasTexel(FogConstants fog, uint3 froxel) {
    uint columns = max(fog.grid.w, 1u);
    return int2(froxel.x + (froxel.z % columns) * fog.grid.x, froxel.y + (froxel.z / columns) * fog.grid.y);
}

// Bilinear in x/y inside one slice, clamped so it never bleeds into the neighbouring slice's tile. `gridUv` spans the
// froxel grid in [0, 1].
float4 fogSampleSlice(Texture2D<float4> atlas, SamplerState linearSampler, FogConstants fog, float2 gridUv, uint slice) {
    float2 grid = float2(fog.grid.xy);
    float2 texel = clamp(gridUv * grid, float2(0.5, 0.5), grid - 0.5);
    uint columns = max(fog.grid.w, 1u);
    float2 atlasTexel = texel + float2(float(slice % columns), float(slice / columns)) * grid;
    return atlas.SampleLevel(linearSampler, atlasTexel / float2(fogAtlasSize(fog)), 0.0);
}

// Linear in depth between slice centres as well: `sliceCoord` is a continuous slice coordinate (centre of slice s at
// s + 0.5).
float4 fogSampleVolume(Texture2D<float4> atlas, SamplerState linearSampler, FogConstants fog, float2 gridUv, float sliceCoord) {
    float lastSlice = float(max(fog.grid.z, 1u) - 1u);
    float s = clamp(sliceCoord - 0.5, 0.0, lastSlice);
    uint s0 = (uint)floor(s);
    uint s1 = min(s0 + 1u, (uint)lastSlice);
    return lerp(fogSampleSlice(atlas, linearSampler, fog, gridUv, s0), fogSampleSlice(atlas, linearSampler, fog, gridUv, s1),
                frac(s));
}

// The camera's (lens-free) world-space ray through a top-left-origin screen uv, and how fast view depth grows along it.
float3 fogViewRay(ShadowLightingGpuData data, float2 uv, out float depthPerDistance) {
    float4 h = mul(float4(uvToNdc(uv), 0.5, 1.0), data.inverseViewProjection);
    float3 world = h.xyz / (abs(h.w) > SFT_EPSILON ? h.w : SFT_EPSILON);
    float3 direction = safeNormalizeOr(world - data.cameraPositionNear.xyz, float3(0.0, 0.0, -1.0));
    depthPerDistance = max(abs(mul(float4(direction, 0.0), data.view).z), 1.0e-3);
    return direction;
}

float fogHash(float3 p) {
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float fogValueNoise(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(fogHash(i + float3(0, 0, 0)), fogHash(i + float3(1, 0, 0)), f.x),
                     lerp(fogHash(i + float3(0, 1, 0)), fogHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(fogHash(i + float3(0, 0, 1)), fogHash(i + float3(1, 0, 1)), f.x),
                     lerp(fogHash(i + float3(0, 1, 1)), fogHash(i + float3(1, 1, 1)), f.x), f.y),
                f.z);
}

// Extinction coefficient (per world unit) at a world position: exponential height fog, optionally broken up by
// wind-scrolled value noise.
float fogDensity(FogConstants fog, float3 position) {
    float aboveBase = position.y - fog.medium.z;
    float density = fog.medium.x * (fog.medium.y > 0.0 ? exp(-fog.medium.y * max(aboveBase, 0.0)) : 1.0);
    if (fog.albedoNoise.w > 0.0) {
        float3 q = (position - fog.windTime.xyz * fog.windTime.w) / max(fog.emissiveNoiseScale.w, 1.0e-3);
        float n = fogValueNoise(q) * 0.65 + fogValueNoise(q * 2.03 + 17.0) * 0.35;
        density *= max(lerp(1.0, n * 2.0, saturate(fog.albedoNoise.w)), 0.0);
    }
    return max(density, 0.0);
}

float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    float denominator = max(1.0 + g2 - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - g2) / (4.0 * SFT_PI * denominator * sqrt(denominator));
}
