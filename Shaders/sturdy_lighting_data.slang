// The deferred lighting constants and the clustered local-light list, shared by every pass that lights something
// (deferred_shadow_lighting.slang, volumetric_fog_*.slang). Layouts mirror `Renderer::ShadowLightingGpuData`,
// `Renderer::LocalLightGpuData` and the cluster buffers built by `Renderer/LightClusters.cpp`; change both sides together.

import sturdy_common;

static const uint SFT_MAX_SPOT_LIGHTS = 8;
static const uint SFT_MAX_POINT_LIGHTS = 8;
static const uint SFT_MAX_SHADOW_VIEWS = 36;

struct ShadowViewGpuData {
    float4x4 viewProjection;
    float4 atlasScaleBias;
    float4 depthParams;
    float4 filterParams;
};

struct DirectionalLightGpuData {
    float4 directionAngularRadius;
    float4 radianceShadow;
    // View-space distance at which each cascade ends.
    float4 cascadeSplits;
    // View-space distance at which each cascade begins cross-fading into the next one.
    float4 cascadeFadeStarts;
    float4 cascadeParams;
};

struct SpotLightGpuData {
    float4 positionRange;
    float4 directionOuterCos;
    float4 radianceInnerCos;
    float4 shadowParams;
};

struct PointLightGpuData {
    float4 positionRange;
    float4 radianceSourceRadius;
    float4 shadowParams;
};

struct ShadowLightingGpuData {
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4x4 view;
    float4 cameraPositionNear;
    float4 ambientRadianceExposure;
    float4 backgroundColor;
    float4 counts;
    float4 shadowParams;
    float4 contactShadowParams;
    // (max intensity, fade-out view distance, camera lens strength, unused)
    float4 contactShadowParamsExtra;
    float4 viewportParams;
    float4 spectralParams;
    DirectionalLightGpuData sun;
    SpotLightGpuData spotLights[SFT_MAX_SPOT_LIGHTS];
    PointLightGpuData pointLights[SFT_MAX_POINT_LIGHTS];
    ShadowViewGpuData shadowViews[SFT_MAX_SHADOW_VIEWS];
    // Clustered lighting: (tiles x, tiles y, depth slices, tile size in pixels)
    float4 clusterGrid;
    // (near, far, slice scale, slice bias): slice = floor(log(view depth) * scale + bias)
    float4 clusterDepth;
    // (1 = clustered lighting on, local light count, max lights per cluster, unused)
    float4 clusterFlags;
    // (environment intensity (0 = none), screen-space reflection intensity (0 = none), 1 = specular occlusion, unused)
    float4 indirectSpecular;
};

// One entry of the clustered local-light list.
struct LocalLightGpuData {
    // xyz position, w range
    float4 positionRange;
    // spot: xyz cone axis, w cos(outer half-angle); point: w = -2
    float4 directionOuterCos;
    // rgb radiance, w cos(inner half-angle) for spots
    float4 radianceInnerCos;
    // x first shadow view (-1 none), y source radius, z 0 = spot / 1 = point
    float4 shadowRadiusKind;
};

// Which of a point light's six shadow faces covers a direction from the light.
uint pointShadowFace(float3 fromLight) {
    float3 a = abs(fromLight);
    if (a.x >= a.y && a.x >= a.z) return fromLight.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.x && a.y >= a.z) return fromLight.y >= 0.0 ? 2u : 3u;
    return fromLight.z >= 0.0 ? 4u : 5u;
}

// Inverse-square falloff windowed smoothly to zero at the light's range.
float punctualAttenuation(float distanceToLight, float range) {
    float ratio = distanceToLight / max(range, SFT_EPSILON);
    float window = saturate(1.0 - pow4(ratio));
    return square(window) / max(distanceToLight * distanceToLight, 0.01);
}

bool clusteredLightingEnabled(ShadowLightingGpuData data) {
    return data.clusterFlags.x > 0.5;
}

// The cluster a world position falls in. The tile comes from the plain (lens-free) view-projection so it matches the
// CPU binning whatever the camera's lens warp does to pixels.
uint lightClusterIndex(ShadowLightingGpuData data, float3 worldPosition) {
    uint tilesX = (uint)data.clusterGrid.x;
    uint tilesY = (uint)data.clusterGrid.y;
    uint slices = (uint)data.clusterGrid.z;
    float tilePx = max(data.clusterGrid.w, 1.0);
    float4 clip = mul(float4(worldPosition, 1.0), data.viewProjection);
    float2 ndc = clip.xy / max(abs(clip.w), 1e-6);
    float2 renderSize = float2(1.0 / max(data.viewportParams.x, 1e-6), 1.0 / max(data.viewportParams.y, 1e-6));
    float2 pixel = float2((ndc.x * 0.5 + 0.5) * renderSize.x, (0.5 - ndc.y * 0.5) * renderSize.y);
    uint x = (uint)clamp(floor(pixel.x / tilePx), 0.0, float(max(tilesX, 1u) - 1u));
    uint y = (uint)clamp(floor(pixel.y / tilePx), 0.0, float(max(tilesY, 1u) - 1u));
    float viewDepth = max(abs(mul(float4(worldPosition, 1.0), data.view).z), data.clusterDepth.x);
    float sliceF = floor(log(viewDepth) * data.clusterDepth.z + data.clusterDepth.w);
    uint slice = (uint)clamp(sliceF, 0.0, float(max(slices, 1u) - 1u));
    return (slice * tilesY + y) * tilesX + x;
}

// Light arriving at `position` from one local light, before shadowing. Returns whether the position is inside the light's
// range (and cone); `lightVector` points from the position towards the light.
bool localLightIncident(LocalLightGpuData light, float3 position, out float3 lightVector, out float distanceToLight,
                               out float3 radiance) {
    float3 toLight = light.positionRange.xyz - position;
    distanceToLight = length(toLight);
    lightVector = toLight / max(distanceToLight, SFT_EPSILON);
    float attenuation = punctualAttenuation(distanceToLight, light.positionRange.w);
    float cone = 1.0;
    if (light.directionOuterCos.w > -1.5) {
        float coneCos = dot(safeNormalizeOr(light.directionOuterCos.xyz, float3(0.0, -1.0, 0.0)), -lightVector);
        cone = smoothstep(light.directionOuterCos.w, light.radianceInnerCos.w, coneCos);
    }
    radiance = light.radianceInnerCos.rgb * attenuation * cone;
    return distanceToLight < light.positionRange.w && cone > 0.0;
}
