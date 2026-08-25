// ReSTIR GI pass 1/4: for every gbuffer pixel, RIS-selects one guided diffuse bounce direction, traces
// it against the scene TLAS, and shades the hit with real multi-light MIS-weighted NEE (sun + up to 8
// colored point/spot lights via sturdy_light_sampling.slang, replacing surfel GI's sun-only NEE), the
// baked Hillaire atmosphere LUTs on ray miss (replacing surfel GI's flat sky color), and a damped read
// of last frame's own final lit scene color reprojected at the hit point for multi-bounce feedback
// (replacing surfel GI's single-bounce-only accumulation). Writes one candidate reservoir per pixel,
// consumed by restir_gi_temporal_reuse.slang.
//
// Two variance-reduction techniques on top of the base algorithm:
//   - Ray guiding: the primary bounce direction is drawn via resampled importance sampling (RIS) among
//     a handful of cosine-hemisphere candidates, biased toward a per-pixel cached "dominant incoming
//     light direction" that is itself updated from what this pass finds each frame and reprojected via
//     motion vectors — the same RIS guiding scheme the deleted surfel GI's ray-trace shader used,
//     carried over here rather than reinvented. RIS's contribution weight exactly compensates for the
//     biased selection, so it is unbiased for any bias strength; with no guide history yet, it reduces
//     algebraically to plain cosine-hemisphere sampling.
//   - Multiple importance sampling (MIS): direct lighting at the traced hit point combines the light-
//     sampling (NEE) and BSDF (cosine-hemisphere) strategies via the balance heuristic — but only for
//     the sun, which has real (if tiny) angular extent and so is a legitimate target for both
//     strategies. Point/spot lights are delta lights with zero solid angle; a scattered ray can never
//     hit one, so no competing BSDF strategy exists for them and plain importance-sampled NEE (no MIS
//     weight) is already the standard, correct treatment — exactly how offline path tracers handle
//     delta lights.
//
// Reservoir weighting here (and in the temporal/spatial passes) uses a simplified target function
// `pHat(y) = luminance(y.radiance)` — the cached outgoing radiance at the sample point, not a full
// re-evaluation of the reusing pixel's own BRDF. This is a standard, deliberate simplification: it
// avoids needing to re-fetch/re-evaluate material data for every reservoir considered during temporal
// and spatial reuse, at the cost of slightly under/over-weighting reuse between very differently
// colored neighboring surfaces. Direction sampling is cosine-weighted against a diffuse (Lambertian/
// E.ON) lobe, matching surfel GI's scope — this pass produces indirect *diffuse* GI, not indirect
// specular (which remains SSR/probe-driven elsewhere in the pipeline).
import sturdy_common;
import sturdy_pbr;
import sturdy_spectral_common;
import sturdy_path_transport;
import sturdy_spectral_scene;
import sturdy_atmosphere;
import sturdy_light_sampling;

struct ReservoirGpuData {
    float4 samplePositionPdf;
    float4 sampleRadianceNormalX;
    float4 normalYValid;
    float4 weightM;
};

struct GuideGpuData {
    float4 directionStrength;
};

struct RestirGiPackedLight {
    float4 positionRange;
    float4 radianceSourceRadius;
};

struct RestirGiFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 cameraPositionFrameIndex;
    float4 extentMaxRayDistance;
    float4 sunDirectionAngularRadius;
    float4 sunRadiance;
    float4 temporalSpatialParams;
    float4 lightCountParams;
    RestirGiPackedLight lights[kRestirGiMaxLights];
};

ConstantBuffer<RestirGiFrameConstants> frame;
ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
Texture2D<float4> gbufferAlbedo;
Texture2D<float4> gbufferMaterial;
Texture2D<float4> gbufferEmissive;
Texture2D<float2> gbufferMotion;
Texture2D<float4> previousSceneColor;
Texture2D<float4> transmittanceLut;
Texture2D<float4> skyViewLut;
SamplerState linearSampler;
SamplerState atmosphereSampler;
RWStructuredBuffer<ReservoirGpuData> reservoirCandidate;
StructuredBuffer<GuideGpuData> guideDirectionPrev;
RWStructuredBuffer<GuideGpuData> guideDirectionCurrent;

static const uint2 SFT_RESTIR_SKY_VIEW_LUT_SIZE = uint2(192, 108);
static const float3 SFT_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
static const uint SFT_RESTIR_GUIDE_CANDIDATE_COUNT = 4u;
static const float SFT_RESTIR_GUIDE_LOBE_EXPONENT = 8.0;
static const float SFT_RESTIR_GUIDE_MAX_BIAS = 6.0;
static const float SFT_RESTIR_GUIDE_BLEND_ALPHA = 0.25;

float3 reconstructWorldPosition(uint2 pixel, uint2 extent, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

// Sampled from the same baked atmosphere LUTs the rasterized sky and spectral path tracer's
// `environmentRgbSplit` use — mirrors that function so a missed GI bounce ray sees the real,
// time-of-day-driven sky instead of a flat constant.
float3 sampleEnvironmentRadiance(float3 direction) {
    AtmosphereParams atmosphere = unpackAtmosphereParams(atmosphereData);
    float3 planetOrigin = atmosphereData.cameraPositionPlanetSpace.xyz;
    float3 sunDirToward = safeNormalizeOr(atmosphereData.sunDirectionAngularRadius.xyz, float3(0.0, 1.0, 0.0));
    float atmosphereViewHeight = max(length(planetOrigin), atmosphere.planetRadiusMeters + 1.0);
    float tGroundUnused;
    bool rayIntersectsGround = distanceToGround(atmosphere, planetOrigin, direction, tGroundUnused);
    float2 skyUv = skyViewLutUv(atmosphere, direction, float3(0.0, 1.0, 0.0), sunDirToward,
                                atmosphereViewHeight, rayIntersectsGround, SFT_RESTIR_SKY_VIEW_LUT_SIZE);
    return skyViewLut.SampleLevel(atmosphereSampler, skyUv, 0.0).rgb;
}

// Damped read of last frame's own final composited lighting at the reprojected hit position — the
// "infinite bounce" trick: because GI feeds its own previous output back in as an extra term at the
// *next* bounce vertex, light keeps propagating across frames instead of stopping after one bounce,
// converging toward a multi-bounce result over a handful of frames without a separate probe cache.
// Damped by `light_count_params.y` (multi-bounce feedback strength, < 1) so the loop cannot runaway-
// amplify, and skipped entirely (returns 0) off-screen, behind the camera, or before any history exists.
float3 sampleMultiBounceFeedback(float3 worldPosition) {
    if (frame.lightCountParams.z < 0.5) return float3(0.0, 0.0, 0.0);
    float4 previousClip = mul(float4(worldPosition, 1.0), frame.previousViewProjection);
    if (previousClip.w <= 1.0e-4) return float3(0.0, 0.0, 0.0);
    float2 previousUv = ndcToUv(previousClip.xy / previousClip.w);
    if (any(previousUv < 0.0) || any(previousUv > 1.0)) return float3(0.0, 0.0, 0.0);
    float3 history = previousSceneColor.SampleLevel(linearSampler, previousUv, 0.0).rgb;
    return max(history, float3(0.0, 0.0, 0.0)) * saturate(frame.lightCountParams.y);
}

// Unnormalized RIS target proxy: 1 everywhere (matching the source cosine distribution's implicit
// weight) plus an extra lobe toward `guideDirection` when a usable cached guide exists. Always > 0,
// which is what makes RIS valid for any `guideBias`.
float evaluateGuideLobe(float3 direction, float3 guideDirection, float guideBias, bool hasGuide) {
    if (!hasGuide) return 1.0;
    float alignment = saturate(dot(direction, guideDirection));
    return 1.0 + guideBias * pow(alignment, SFT_RESTIR_GUIDE_LOBE_EXPONENT);
}

float3 shadeHit(SurfaceSample surface, float3 viewDirection, uint instanceMask, inout uint rng) {
    SturdyPackedLight sturdyLights[kRestirGiMaxLights];
    [unroll]
    for (uint j = 0; j < kRestirGiMaxLights; ++j) {
        sturdyLights[j].positionRange = frame.lights[j].positionRange;
        sturdyLights[j].radianceSourceRadius = frame.lights[j].radianceSourceRadius;
    }

    float3 sunDirection = normalize(-frame.sunDirectionAngularRadius.xyz);
    float sunAngularRadius = frame.sunDirectionAngularRadius.w;
    uint lightCount = (uint)frame.lightCountParams.x;
    float selectionProbability = 1.0 / (1.0 + (float)min(lightCount, kRestirGiMaxLights));
    float pdfLightForSun = selectionProbability * sunDirectionPdf(sunAngularRadius);

    // --- Light-sampling (NEE) strategy: one candidate light, MIS-weighted only for the sun. ---
    LightSampleResult picked = sampleOneLightUniform(
        surface.position, sunDirection, frame.sunRadiance.rgb, sunAngularRadius,
        sturdyLights, lightCount, rng);

    float3 direct = float3(0.0, 0.0, 0.0);
    if (picked.pdf > 0.0) {
        float visibility = traceSpectralVisibility(
            surface.position + surface.shadingNormal * 0.001, picked.direction, picked.distance, instanceMask);
        float cosine = saturate(dot(surface.shadingNormal, picked.direction));
        float3 diffuseBrdf = evaluateEonDiffuse(
            surface.material.baseColor.rgb, surface.material.surface.x,
            surface.shadingNormal, picked.direction, viewDirection);
        float misWeight = picked.isSun
            ? balanceHeuristicWeight(picked.pdf, diffuseCosinePdf(surface.shadingNormal, picked.direction))
            : 1.0;
        direct = diffuseBrdf * (1.0 - surface.material.surface.y) * cosine * picked.radiance
               * (misWeight * visibility / picked.pdf);
    }

    // --- BSDF-sampling strategy's reciprocal MIS term, sun only (see file header): draw one cosine-
    // sampled direction and, only on the rare occasion it actually lands within the sun's disk, spend a
    // second visibility ray to add its MIS-weighted contribution. ---
    if (pdfLightForSun > 0.0) {
        float3 bsdfDirection = cosineHemisphere(surface.shadingNormal, rng);
        if (dot(bsdfDirection, sunDirection) > cos(sunAngularRadius)) {
            float pdfBsdf = diffuseCosinePdf(surface.shadingNormal, bsdfDirection);
            if (pdfBsdf > 0.0) {
                float visibility = traceSpectralVisibility(
                    surface.position + surface.shadingNormal * 0.001, bsdfDirection, 1.0e6, instanceMask);
                float misWeight = balanceHeuristicWeight(pdfBsdf, pdfLightForSun);
                float3 diffuseBrdf = evaluateEonDiffuse(
                    surface.material.baseColor.rgb, surface.material.surface.x,
                    surface.shadingNormal, bsdfDirection, viewDirection);
                float cosine = saturate(dot(surface.shadingNormal, bsdfDirection));
                direct += diffuseBrdf * (1.0 - surface.material.surface.y) * (cosine / pdfBsdf)
                        * frame.sunRadiance.rgb * (misWeight * visibility);
            }
        }
    }

    float3 bounce = sampleMultiBounceFeedback(surface.position) * surface.material.baseColor.rgb;

    return surface.material.emissiveAndStrength.rgb * surface.material.emissiveAndStrength.a + direct + bounce;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void initialSampleMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= extent)) return;
    uint pixelIndex = dispatchThreadId.y * extent.x + dispatchThreadId.x;

    float depth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    if (depth >= 0.999999) {
        reservoirCandidate[pixelIndex] = (ReservoirGpuData)0;
        guideDirectionCurrent[pixelIndex].directionStrength = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float3 worldPosition = reconstructWorldPosition(dispatchThreadId.xy, extent, depth);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));
    float3 viewDirection = safeNormalizeOr(frame.cameraPositionFrameIndex.xyz - worldPosition, normal);

    // Reproject last frame's per-pixel guide direction via the motion vector — the same screen-space
    // reprojection restir_gi_temporal_reuse.slang uses for reservoirs.
    float3 guideDirection = float3(0.0, 0.0, 0.0);
    float guideStrengthRaw = 0.0;
    if (frame.lightCountParams.z > 0.5) {
        float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(extent);
        float2 motion = gbufferMotion.Load(int3(dispatchThreadId.xy, 0));
        float2 previousUv = uv - motion;
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            int2 previousPixel = clamp(int2(previousUv * float2(extent)), int2(0, 0), int2(extent) - int2(1, 1));
            uint previousIndex = (uint)previousPixel.y * extent.x + (uint)previousPixel.x;
            float4 packed = guideDirectionPrev[previousIndex].directionStrength;
            guideDirection = packed.xyz;
            guideStrengthRaw = packed.w;
        }
    }

    uint rng = spectralHash(pixelIndex ^ ((uint)frame.cameraPositionFrameIndex.w * 2654435761u));

    // Self-scaling normalization: express the cached mean luminance relative to the sun's radiance
    // magnitude (the dominant light source in this engine) rather than an absolute constant, so the
    // guiding strength behaves consistently across scenes/exposures instead of needing per-scene tuning
    // — the same scheme the deleted surfel GI ray-trace shader used.
    float sunReference = max(length(frame.sunRadiance.rgb), SFT_EPSILON);
    float guideBias = saturate(guideStrengthRaw / (guideStrengthRaw + sunReference * 0.05)) * SFT_RESTIR_GUIDE_MAX_BIAS;
    bool hasGuide = guideBias > SFT_EPSILON && dot(guideDirection, guideDirection) > 0.25;

    // RIS: draw a handful of candidate directions from the cheap, unbiased cosine distribution, then
    // resample one proportional to the (unnormalized) guide lobe weight. Only the resampled direction
    // is actually traced.
    float3 candidateDirections[SFT_RESTIR_GUIDE_CANDIDATE_COUNT];
    float candidateLobes[SFT_RESTIR_GUIDE_CANDIDATE_COUNT];
    float lobeSum = 0.0;
    [unroll]
    for (uint c = 0; c < SFT_RESTIR_GUIDE_CANDIDATE_COUNT; ++c) {
        candidateDirections[c] = cosineHemisphere(normal, rng);
        float lobe = evaluateGuideLobe(candidateDirections[c], guideDirection, guideBias, hasGuide);
        candidateLobes[c] = lobe;
        lobeSum += lobe;
    }
    float selector = spectralRandom(rng) * lobeSum;
    uint selected = SFT_RESTIR_GUIDE_CANDIDATE_COUNT - 1u;
    float cumulative = 0.0;
    [unroll]
    for (uint c = 0; c < SFT_RESTIR_GUIDE_CANDIDATE_COUNT; ++c) {
        cumulative += candidateLobes[c];
        if (selector <= cumulative) { selected = c; break; }
    }
    float3 direction = candidateDirections[selected];
    float selectedLobe = max(candidateLobes[selected], SFT_EPSILON);
    // RIS unbiased contribution weight: the cosine-hemisphere source pdf and the diffuse Lambertian
    // target's cosine factor cancel exactly (see restir_gi_shade_resolve.slang / the reservoir's own
    // W==1 comment below), so this reduces to a pure lobe-ratio scale on the traced radiance — exactly
    // 1 (no-op) whenever `hasGuide` is false, since every `candidateLobes[c]` is then 1.
    float risWeight = lobeSum / (float(SFT_RESTIR_GUIDE_CANDIDATE_COUNT) * selectedLobe);

    SpectralRayHit hit;
    SurfaceSample surface;
    float3 radiance;
    float3 hitPosition = worldPosition;
    float3 hitNormal = normal;
    bool valid = false;
    if (traceSpectralClosestSurface(
            worldPosition + normal * 0.01, direction, 0.01, frame.extentMaxRayDistance.w,
            0xffu, hit, surface)) {
        radiance = shadeHit(surface, -direction, 0xffu, rng);
        hitPosition = surface.position;
        hitNormal = surface.shadingNormal;
        valid = true;
    } else {
        radiance = sampleEnvironmentRadiance(direction);
        hitPosition = worldPosition + direction * frame.extentMaxRayDistance.w;
        hitNormal = -direction;
        valid = true;
    }
    radiance *= risWeight;

    // Single-sample reservoir: pHat(y) = luminance(radiance), p(y) = 1 (the cosine-hemisphere direction
    // pdf cancels algebraically against the Lambertian target the same way surfel_ray_trace.slang's
    // plain cosine average already relied on; the RIS weight above already folds in the guiding bias
    // correction), so this candidate's own W is exactly 1.
    float2 encodedNormal = encodeOctahedralNormal(hitNormal);
    ReservoirGpuData reservoir;
    reservoir.samplePositionPdf = float4(hitPosition, 1.0);
    reservoir.sampleRadianceNormalX = float4(radiance, encodedNormal.x);
    reservoir.normalYValid = float4(encodedNormal.y, valid ? 1.0 : 0.0, 0.0, 0.0);
    reservoir.weightM = float4(max(dot(radiance, SFT_LUMINANCE_WEIGHTS), 0.0), valid ? 1.0 : 0.0, 1.0, 0.0);
    reservoirCandidate[pixelIndex] = reservoir;

    // Update the guide cache for next frame from what this sample actually found.
    float luminance = max(dot(radiance, SFT_LUMINANCE_WEIGHTS), 0.0);
    float3 measuredDirection = luminance > SFT_EPSILON ? direction : (hasGuide ? guideDirection : normal);
    float3 blendedGuideDirection = safeNormalizeOr(
        lerp(hasGuide ? guideDirection : measuredDirection, measuredDirection, SFT_RESTIR_GUIDE_BLEND_ALPHA),
        measuredDirection);
    float blendedGuideStrength = lerp(guideStrengthRaw, luminance, SFT_RESTIR_GUIDE_BLEND_ALPHA);
    guideDirectionCurrent[pixelIndex].directionStrength = float4(blendedGuideDirection, blendedGuideStrength);
}
