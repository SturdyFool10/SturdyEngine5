// Compute ray-query integrators for all experimental modes. FullPathTracing is the only entry point
// that replaces primary raster visibility; its first hit writes the canonical deferred G-buffer ABI.

import sturdy_common;
import sturdy_pbr;
import sturdy_spectral_common;
import sturdy_path_transport;
import sturdy_spectral_scene;
import sturdy_spectral_photons;
import sturdy_atmosphere;

struct SpectralFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4 cameraPosition;
    float4 sunDirectionAndAngularRadius;
    float4 sunRadiance;
    uint2 extent;
    uint frameIndex;
    uint samplesPerPixel;
    uint maxBounces;
    uint russianRouletteStartBounce;
    uint aoRayCount;
    uint instanceMask;
    float aoRadius;
    float wavelengthMinNm;
    float wavelengthMaxNm;
    float environmentIntensity;
    // x gather radius, y photon voxel size, zw reserved.
    float4 causticGatherParameters;
    // x hash capacity, y chain traversal limit, z enabled, w photon capacity.
    uint4 causticGatherControl;
    // x reset history, y maximum accumulated frame weight, zw reserved.
    uint4 accumulationControl;
};

ConstantBuffer<SpectralFrameConstants> frame;
StructuredBuffer<SpectralPhoton> causticPhotons;
StructuredBuffer<uint> causticPhotonCount;
StructuredBuffer<uint> causticPhotonHashHeads;

// Real procedural sky for miss rays — same baked LUTs + physics buffer
// Shaders/deferred_shadow_lighting.slang resolves the rasterized sky/aerial-perspective from (see
// that shader and sturdy_atmosphere.slang's doc comment: every function there is a pure function of
// its explicit ray origin/direction and bound LUTs, deliberately written so a raytracing miss case
// like this one calls the exact same math instead of re-deriving its own sky model).
ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float4> transmittanceLut;
Texture2D<float4> skyViewLut;
SamplerState atmosphereSampler;

Texture2D<float4> rasterAlbedo;
Texture2D<float2> rasterNormal;
Texture2D<float4> rasterMaterial;
Texture2D<float4> rasterEmissive;
Texture2D<float2> rasterMotion;
Texture2D<float> rasterDepth;

RWTexture2D<float4> effectOutput;
RWTexture2D<float4> sceneColorOutput;
RWTexture2D<float2> gbufferMotionOutput;
RWTexture2D<float> primaryDepthOutput;
RWTexture2D<float4> accumulationOutput;

float3 reconstructWorldPosition(uint2 pixel, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(frame.extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

void primaryRay(uint2 pixel, inout uint rng, out float3 origin, out float3 direction) {
    float2 jitter = float2(spectralRandom(rng), spectralRandom(rng)) - 0.5;
    float2 uv = (float2(pixel) + 0.5 + jitter) / float2(frame.extent);
    float4 farClip = float4(uvToNdc(uv), 1.0, 1.0);
    float4 farWorld = mul(farClip, frame.inverseViewProjection);
    origin = frame.cameraPosition.xyz;
    direction = normalize(farWorld.xyz / max(farWorld.w, 1.0e-6) - origin);
}

// Must match sky_view_lut.slang's LUT_SIZE exactly — same mirrored-constant convention
// Shaders/deferred_shadow_lighting.slang already uses for its own copy of this constant.
static const uint2 SFT_SKY_VIEW_LUT_SIZE = uint2(192, 108);

// Real procedural sky, sampled from the same baked atmosphere LUTs the rasterized background uses
// (see Shaders/deferred_shadow_lighting.slang's fragmentMain miss case, which this mirrors). The
// planet-space reference height/position always uses the camera, not the true miss-ray origin — an
// approximation the rasterized pass already makes and is accurate here too: the atmosphere's spatial
// scale is planetary, so a scene-local ray-origin offset (even several bounces deep) is negligible.
// Sky and sun-disk radiance kept separate so a caller that also does NEE toward the sun (traceSpectralPath)
// can MIS-weight only the sun-disk term — the sky gradient has no NEE counterpart (it's never estimated
// any other way), but the disk is a second, redundant estimator of the exact same sunIlluminance quantity
// directSun's closed-form NEE already accounts for (see sunDiskLuminance's own doc comment).
struct EnvironmentSample {
    float3 sky;
    float3 sunDisk;
};

// Real procedural sky, sampled from the same baked atmosphere LUTs the rasterized background uses
// (see Shaders/deferred_shadow_lighting.slang's fragmentMain miss case, which this mirrors). The
// planet-space reference height/position always uses the camera, not the true miss-ray origin — an
// approximation the rasterized pass already makes and is accurate here too: the atmosphere's spatial
// scale is planetary, so a scene-local ray-origin offset (even several bounces deep) is negligible.
EnvironmentSample environmentRgbSplit(float3 direction) {
    AtmosphereParams atmosphere = unpackAtmosphereParams(atmosphereData);
    float3 planetOrigin = atmosphereData.cameraPositionPlanetSpace.xyz;
    float3 sunDirToward = safeNormalizeOr(atmosphereData.sunDirectionAngularRadius.xyz, float3(0.0, 1.0, 0.0));
    float atmosphereViewHeight = max(length(planetOrigin), atmosphere.planetRadiusMeters + 1.0);

    float tGroundUnused;
    bool rayIntersectsGround = distanceToGround(atmosphere, planetOrigin, direction, tGroundUnused);
    float2 skyUv = skyViewLutUv(atmosphere, direction, float3(0.0, 1.0, 0.0), sunDirToward,
                                atmosphereViewHeight, rayIntersectsGround, SFT_SKY_VIEW_LUT_SIZE);
    EnvironmentSample result;
    result.sky = skyViewLut.SampleLevel(atmosphereSampler, skyUv, 0.0).rgb;
    float3 upVector = planetOrigin * safeRcp(atmosphereViewHeight);
    float3 transmittanceToSpace = sampleTransmittanceLut(transmittanceLut, atmosphereSampler, atmosphere,
                                                          atmosphereViewHeight, dot(upVector, direction));
    result.sunDisk = sunDiskLuminance(direction, sunDirToward, atmosphereData.sunDirectionAngularRadius.w,
                            atmosphereData.sunIlluminance.rgb, transmittanceToSpace);
    return result;
}

float3 environmentRgb(float3 direction) {
    EnvironmentSample sample = environmentRgbSplit(direction);
    return (sample.sky + sample.sunDisk) * frame.environmentIntensity;
}

float directSun(
    SurfaceSample surface, float3 viewDirection, float wavelengthNm, uint instanceMask) {
    float transmission = saturate(surface.material.transmission.x);
    if (transmission >= 0.999) {
        return 0.0;
    }
    float3 lightDirection = normalize(-frame.sunDirectionAndAngularRadius.xyz);
    float visibility = traceSpectralTransmittance(
        surface.position + surface.shadingNormal * 0.001,
        lightDirection, 1.0e6, instanceMask, wavelengthNm, 16u);
    float reflectance = rgbReflectanceAtWavelength(surface.material.baseColor.rgb, wavelengthNm);
    float irradiance = rgbSpectrumAtWavelength(frame.sunRadiance.rgb, wavelengthNm);
    float brdf = evaluateOpaquePbr(
        surface.shadingNormal, viewDirection, lightDirection, reflectance,
        surface.material.surface.x, surface.material.surface.y, surface.material.surface.w);
    return irradiance * visibility * brdf * (1.0 - transmission);
}

float3 traceSpectralPath(uint2 pixel, inout uint rng, out SurfaceSample primarySurface, out float primaryDistance) {
    SpectralSample spectral = sampleHeroWavelength(rng, frame.wavelengthMinNm, frame.wavelengthMaxNm);
    float3 rayOrigin;
    float3 rayDirection;
    primaryRay(pixel, rng, rayOrigin, rayDirection);
    float throughput = 1.0;
    float scalarRadiance = 0.0;
    float activeAbsorption = 0.0;
    primarySurface = (SurfaceSample)0;
    primaryDistance = 1.0;

    // MIS between directSun's closed-form NEE term and a BSDF-sampled continuation ray that happens
    // to land on the sun's disk in the environment map — both are unbiased estimators of the same
    // sunIlluminance quantity (see sunDiskLuminance's doc comment), so without this they'd add at full
    // weight each, roughly doubling the sun's contribution on every non-dielectric bounce whose
    // continuation ray lands back on the sun (a common source of fireflies/over-bright specular sun
    // reflections). previousOpaqueBsdfPdfAtSun starts at 0.0, which correctly gives the primary/camera
    // ray's own sun-disk hit (bounce 0) full weight — there's no preceding NEE to compete with it.
    float previousOpaqueBsdfPdfAtSun = 0.0;
    float sunLightPdf = sunSolidAnglePdf(frame.sunDirectionAndAngularRadius.w);

    for (uint bounce = 0; bounce < frame.maxBounces; ++bounce) {
        SpectralRayHit hit;
        SurfaceSample surface;
        if (!traceSpectralClosestSurface(
                rayOrigin, rayDirection, 0.001, 1.0e6, frame.instanceMask, hit, surface)) {
            EnvironmentSample environment = environmentRgbSplit(rayDirection);
            float misWeight = bounce == 0u ? 1.0
                : previousOpaqueBsdfPdfAtSun / max(previousOpaqueBsdfPdfAtSun + sunLightPdf, 1.0e-8);
            float3 weighted = (environment.sky + environment.sunDisk * misWeight) * frame.environmentIntensity;
            scalarRadiance += throughput * rgbSpectrumAtWavelength(weighted, spectral.wavelengthNm);
            break;
        }

        throughput *= beerLambert(activeAbsorption, hit.distance);
        if (bounce == 0u) {
            primarySurface = surface;
            primaryDistance = hit.distance;
        }

        scalarRadiance += throughput * rgbSpectrumAtWavelength(
            surface.material.emissiveAndStrength.rgb * surface.material.emissiveAndStrength.a,
            spectral.wavelengthNm);

        float reflectance = rgbReflectanceAtWavelength(
            surface.material.baseColor.rgb, spectral.wavelengthNm);
        float3 sunLightDirection = normalize(-frame.sunDirectionAndAngularRadius.xyz);
        float opaqueBsdfPdfAtSun = opaquePbrPdf(
            surface.shadingNormal, -rayDirection, sunLightDirection, reflectance,
            surface.material.surface.x, surface.material.surface.y, surface.material.surface.w);
        float neeWeight = sunLightPdf / max(sunLightPdf + opaqueBsdfPdfAtSun, 1.0e-8);
        scalarRadiance += throughput * neeWeight * directSun(
            surface, -rayDirection, spectral.wavelengthNm, frame.instanceMask);

        float transmission = saturate(surface.material.transmission.x);
        bool chooseDielectric = transmission > 0.0 && spectralRandom(rng) < transmission;
        if (chooseDielectric) {
            float eta = cauchyIor(spectral.wavelengthNm,
                                  surface.material.transmission.y,
                                  surface.material.transmission.z);
            DielectricScatter scatter = sampleDielectricDirection(
                rayDirection, surface.outwardGeometricNormal, surface.outwardShadingNormal,
                surface.frontFace, 1.0, eta, rng);
            if (scatter.transmitted) {
                // Camera/radiance transport carries eta^2. Photon/light transport intentionally does not.
                throughput *= scatter.etaRatio * scatter.etaRatio;
                activeAbsorption = scatter.entering
                    ? max(surface.material.transmission.w, 0.0) : 0.0;
            }
            rayDirection = scatter.direction;
            rayOrigin = surface.position + rayDirection * 0.001;
            // Dielectric transmission/specular reflection isn't modeled by directSun's opaque-only NEE
            // at all, so a subsequent sun-disk hit along this ray is not a redundant estimate — full
            // weight (0 here makes misWeight above evaluate to sunLightPdf / (sunLightPdf + 0) = 1.0).
            previousOpaqueBsdfPdfAtSun = 0.0;
        } else {
            previousOpaqueBsdfPdfAtSun = opaqueBsdfPdfAtSun;
            OpaquePbrScatter scatter = sampleOpaquePbrDirection(
                rayDirection, surface.shadingNormal, reflectance,
                surface.material.surface.x, surface.material.surface.y,
                surface.material.surface.w, rng);
            throughput *= scatter.throughputWeight;
            rayDirection = scatter.direction;
            rayOrigin = surface.position + rayDirection * 0.001;
        }

        if (russianRoulette(throughput, bounce, frame.russianRouletteStartBounce, rng) == 0.0) {
            break;
        }
    }
    return max(scalarRadiance, 0.0) * spectral.linearRgbWeight;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shadowOnlyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= frame.extent)) return;
    uint2 pixel = dispatchThreadId.xy;
    float depth = rasterDepth[pixel];
    if (depth >= 0.999999) { effectOutput[pixel] = float4(1.0); return; }
    float3 position = reconstructWorldPosition(pixel, depth);
    float3 normal = decodeOctahedralNormal(rasterNormal[pixel]);
    float visibility = traceSpectralVisibility(
        position + normal * 0.001, normalize(-frame.sunDirectionAndAngularRadius.xyz),
        1.0e6, frame.instanceMask);
    effectOutput[pixel] = float4(visibility, visibility, visibility, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ambientOcclusionOnlyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= frame.extent)) return;
    uint2 pixel = dispatchThreadId.xy;
    float depth = rasterDepth[pixel];
    if (depth >= 0.999999) { effectOutput[pixel] = float4(1.0); return; }
    uint rng = spectralHash(pixel.x + pixel.y * frame.extent.x + frame.frameIndex * 0x9e3779b9u);
    float3 position = reconstructWorldPosition(pixel, depth);
    float3 normal = decodeOctahedralNormal(rasterNormal[pixel]);
    float ao = traceSpectralAmbientOcclusion(
        position, normal, frame.aoRadius, frame.aoRayCount, frame.instanceMask, rng);
    effectOutput[pixel] = float4(ao, ao, ao, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void reflectionOnlyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= frame.extent)) return;
    uint2 pixel = dispatchThreadId.xy;
    float depth = rasterDepth[pixel];
    if (depth >= 0.999999) { effectOutput[pixel] = float4(0.0); return; }
    uint rng = spectralHash(pixel.x + pixel.y * frame.extent.x + frame.frameIndex * 0x85ebca6bu);
    float3 position = reconstructWorldPosition(pixel, depth);
    float3 normal = decodeOctahedralNormal(rasterNormal[pixel]);
    float3 incident = normalize(position - frame.cameraPosition.xyz);
    float roughness = rasterMaterial[pixel].x;
    float3 direction = reflectedDirection(incident, normal, roughness, rng);
    SpectralRayHit hit;
    SurfaceSample surface;
    float3 radiance = environmentRgb(direction);
    if (traceSpectralClosestSurface(
            position + normal * 0.001, direction, 0.001, 1.0e6, frame.instanceMask, hit, surface)) {
        float3 lightDirection = normalize(-frame.sunDirectionAndAngularRadius.xyz);
        float visibility = traceSpectralVisibility(
            surface.position + surface.shadingNormal * 0.001,
            lightDirection, 1.0e6, frame.instanceMask);
        float cosine = saturate(dot(surface.shadingNormal, lightDirection));
        float3 diffuseBrdf = evaluateEonDiffuse(
            surface.material.baseColor.rgb, surface.material.surface.x,
            surface.shadingNormal, lightDirection, -direction);
        float3 diffuse = diffuseBrdf * (1.0 - surface.material.surface.y) * cosine;
        radiance = surface.material.emissiveAndStrength.rgb * surface.material.emissiveAndStrength.a
                 + frame.sunRadiance.rgb * diffuse * visibility
                 + surface.material.baseColor.rgb * 0.03;
    }
    effectOutput[pixel] = float4(radiance, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shadowTransmissionMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= frame.extent)) return;
    uint2 pixel = dispatchThreadId.xy;
    float depth = rasterDepth[pixel];
    if (depth >= 0.999999) { effectOutput[pixel] = float4(1.0); return; }
    uint rng = spectralHash(pixel.x + pixel.y * frame.extent.x + frame.frameIndex * 0xc2b2ae35u);
    SpectralSample spectral = sampleHeroWavelength(rng, frame.wavelengthMinNm, frame.wavelengthMaxNm);
    float3 position = reconstructWorldPosition(pixel, depth);
    float3 normal = decodeOctahedralNormal(rasterNormal[pixel]);
    float3 direction = normalize(-frame.sunDirectionAndAngularRadius.xyz);
    float transmittance = traceSpectralTransmittance(
        position + normal * 0.001, direction, 1.0e6, frame.instanceMask,
        spectral.wavelengthNm, 16u);
    effectOutput[pixel] = float4(max(spectral.linearRgbWeight, 0.0) * transmittance, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void fullPathTracingMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= frame.extent)) return;
    uint2 pixel = dispatchThreadId.xy;
    float3 radiance = float3(0.0);
    SurfaceSample primary = (SurfaceSample)0;
    float primaryDistance = 1.0;
    uint sampleCount = max(frame.samplesPerPixel, 1u);
    for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        uint rng = spectralHash(pixel.x + pixel.y * frame.extent.x
                              + (frame.frameIndex * sampleCount + sampleIndex) * 0x9e3779b9u);
        SurfaceSample samplePrimary;
        float sampleDistance;
        radiance += traceSpectralPath(pixel, rng, samplePrimary, sampleDistance);
        if (sampleIndex == 0u) { primary = samplePrimary; primaryDistance = sampleDistance; }
    }

    float3 caustics = float3(0.0);
    if (frame.causticGatherControl.z != 0u && primary.valid) {
        caustics = gatherSpectralCausticsRgb(
            causticPhotons, causticPhotonCount, causticPhotonHashHeads,
            primary, safeNormalizeOr(frame.cameraPosition.xyz - primary.position, primary.shadingNormal),
            frame.causticGatherParameters.x, frame.causticGatherParameters.y,
            frame.causticGatherControl.w, frame.causticGatherControl.x,
            frame.causticGatherControl.y);
    }
    float3 currentColor = max(radiance / float(sampleCount) + caustics, 0.0);
    float4 history = frame.accumulationControl.x != 0u
        ? float4(0.0) : accumulationOutput[pixel];
    float maximumWeight = max(float(frame.accumulationControl.y), 1.0);
    float previousWeight = clamp(history.a, 0.0, maximumWeight);
    float3 accumulatedColor;
    float nextWeight;
    if (previousWeight < maximumWeight) {
        nextWeight = previousWeight + 1.0;
        accumulatedColor = (history.rgb * previousWeight + currentColor) / nextWeight;
    } else {
        nextWeight = maximumWeight;
        accumulatedColor = lerp(history.rgb, currentColor, 1.0 / maximumWeight);
    }
    accumulationOutput[pixel] = float4(accumulatedColor, nextWeight);
    sceneColorOutput[pixel] = float4(accumulatedColor, 1.0);

    if (!primary.valid) {
        gbufferMotionOutput[pixel] = float2(0.0);
        primaryDepthOutput[pixel] = 1.0;
        return;
    }

    SpectralSceneInstance instance = sceneInstances[primary.instanceIndex];
    float4 clip = mul(float4(primary.position, 1.0), frame.viewProjection);
    float4 previousWorld = mul(float4(primary.objectPosition, 1.0), instance.previousModel);
    float4 previousClip = mul(previousWorld, frame.previousViewProjection);
    float2 currentNdc = clip.xy / max(clip.w, 1.0e-6);
    float2 previousNdc = previousClip.xy / max(previousClip.w, 1.0e-6);
    gbufferMotionOutput[pixel] = (currentNdc - previousNdc) * 0.5;
    primaryDepthOutput[pixel] = saturate(clip.z / max(clip.w, 1.0e-6));
}
