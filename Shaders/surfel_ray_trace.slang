// Casts SurfelGiSettings::rays_per_surfel_per_frame (halved per cascade outward) cosine-weighted
// hemisphere rays from each live surfel against the scene TLAS, evaluates single-bounce direct
// lighting at the hit (mirroring spectral_integrators.slang's reflectionOnlyMain hit-shading, so
// surfel GI stays visually consistent with the existing spectral path tracer), and temporally
// accumulates the result into the surfel's irradiance via an exponential moving average.
import sturdy_common;
import sturdy_pbr;
import sturdy_spectral_common;
import sturdy_path_transport;
import sturdy_spectral_scene;

struct SurfelGpuData {
    float4 positionRadius;
    float4 normalCascade;
    float4 irradianceConfidence;
};

struct SurfelGiFrameConstants {
    float4x4 inverseViewProjection;
    float4 cameraPositionMaxSurfels;
    float4 cascadeDistances;
    float4 gridParams;
    float4 spawnParams;
    float4 temporalIntensity;
    float4 extentFrameIndex;
    float4 sunDirectionAngularRadius;
    float4 sunRadiance;
    float4 skyColorIntensity;
};

ConstantBuffer<SurfelGiFrameConstants> frame;
RWStructuredBuffer<SurfelGpuData> surfelBuffer;

float3 surfelSkyRadiance() {
    return frame.skyColorIntensity.rgb * frame.skyColorIntensity.w;
}

float3 evaluateSurfelHitRadiance(SurfaceSample surface, float3 viewDirection, uint instanceMask) {
    float3 lightDirection = normalize(-frame.sunDirectionAngularRadius.xyz);
    float visibility = traceSpectralVisibility(
        surface.position + surface.shadingNormal * 0.001, lightDirection, 1.0e6, instanceMask);
    float cosine = saturate(dot(surface.shadingNormal, lightDirection));
    float3 diffuseBrdf = evaluateEonDiffuse(
        surface.material.baseColor.rgb, surface.material.surface.x,
        surface.shadingNormal, lightDirection, viewDirection);
    float3 diffuse = diffuseBrdf * (1.0 - surface.material.surface.y) * cosine;
    return surface.material.emissiveAndStrength.rgb * surface.material.emissiveAndStrength.a
         + frame.sunRadiance.rgb * diffuse * visibility
         + surface.material.baseColor.rgb * surfelSkyRadiance() * 0.5;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void rayTraceMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint maxSurfels = (uint)frame.cameraPositionMaxSurfels.w;
    if (dispatchThreadId.x >= maxSurfels) return;

    SurfelGpuData surfel = surfelBuffer[dispatchThreadId.x];
    if (surfel.positionRadius.w <= 0.0) return;

    float cameraDistance = distance(surfel.positionRadius.xyz, frame.cameraPositionMaxSurfels.xyz);
    uint cascadeIndex = 0u;
    if (cameraDistance > frame.cascadeDistances.x) cascadeIndex = 1u;
    if (cameraDistance > frame.cascadeDistances.y) cascadeIndex = 2u;
    if (cameraDistance > frame.cascadeDistances.z) cascadeIndex = 3u;
    uint baseRayCount = max(1u, (uint)frame.spawnParams.z);
    uint rayCount = max(1u, baseRayCount >> cascadeIndex);

    uint rng = spectralHash(dispatchThreadId.x ^ ((uint)frame.extentFrameIndex.z * 2654435761u));
    float3 normal = surfel.normalCascade.xyz;
    float3 accumulated = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < rayCount; ++i) {
        float3 direction = cosineHemisphere(normal, rng);
        SpectralRayHit hit;
        SurfaceSample surface;
        float3 radiance;
        if (traceSpectralClosestSurface(
                surfel.positionRadius.xyz + normal * 0.01, direction, 0.01, frame.spawnParams.w,
                0xffu, hit, surface)) {
            radiance = evaluateSurfelHitRadiance(surface, -direction, 0xffu);
        } else {
            radiance = surfelSkyRadiance();
        }
        accumulated += radiance;
    }
    accumulated /= (float)rayCount;

    float alpha = saturate(frame.temporalIntensity.x);
    float confidence = surfel.irradianceConfidence.w;
    bool firstSample = confidence <= 0.0;
    float3 blended = firstSample ? accumulated : lerp(surfel.irradianceConfidence.rgb, accumulated, alpha);
    surfelBuffer[dispatchThreadId.x].irradianceConfidence = float4(blended, min(confidence + alpha, 1.0));
}
