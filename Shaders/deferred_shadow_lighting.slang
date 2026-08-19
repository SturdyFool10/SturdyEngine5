// Physically based deferred lighting and the raster shadow resolve.
//
// Shadow generation is a single depth atlas pass. This resolve supports stable cascaded sun
// shadows, perspective spot maps, six-face point maps, cascade cross-fades, receiver normal bias,
// and a bounded shadow-space-stable PCSS blocker-search/filter. All loops have fixed compile-time
// maxima so cost is deterministic and the shader remains portable across Vulkan/D3D12/Metal-class hardware.

import sturdy_common;
import sturdy_pbr;
import sturdy_atmosphere;

// Must match sky_view_lut.slang's LUT_SIZE exactly — same mirrored-constant convention as the limits
// below (no shared header between C++/multiple shaders to source a single definition from).
static const uint2 SFT_SKY_VIEW_LUT_SIZE = uint2(192, 108);

static const uint SFT_MAX_SPOT_LIGHTS = 8;
static const uint SFT_MAX_POINT_LIGHTS = 8;
static const uint SFT_MAX_SHADOW_VIEWS = 36;
static const uint SFT_PCSS_BLOCKER_SAMPLES = 12;
static const uint SFT_PCSS_FILTER_SAMPLES = 8;
static const float SFT_MIN_PBR_ROUGHNESS = 0.045;

static const float2 SFT_BLOCKER_DISK[SFT_PCSS_BLOCKER_SAMPLES] = {
    float2( 0.00000000,  0.00000000), float2( 0.21320072,  0.00000000),
    float2(-0.27229150,  0.24944132), float2( 0.04167858, -0.47490591),
    float2( 0.34320580,  0.44765120), float2(-0.62982486, -0.11140715),
    float2( 0.59662509, -0.37952405), float2(-0.19955942,  0.74235108),
    float2(-0.38058145, -0.73278642), float2( 0.82570951,  0.30154779),
    float2(-0.85901316,  0.35458815), float2( 0.41410111, -0.88491001)
};

static const float2 SFT_FILTER_DISK[SFT_PCSS_FILTER_SAMPLES] = {
    float2( 0.25000000,  0.00000000), float2(-0.31929009,  0.29249588),
    float2( 0.04887247, -0.55687654), float2( 0.40244448,  0.52491756),
    float2(-0.73853511, -0.13063646), float2( 0.69960493, -0.44503139),
    float2(-0.23400416,  0.87048380), float2(-0.44627131, -0.85926825)
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct ShadowViewGpuData {
    float4x4 viewProjection;
    float4 atlasScaleBias;
    float4 depthParams;
    float4 filterParams;
};

struct DirectionalLightGpuData {
    float4 directionAngularRadius;
    float4 radianceShadow;
    float4 cascadeSplits;
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
    float4 gtaoParams;
    float4 viewportParams;
    float4 spectralParams;
    DirectionalLightGpuData sun;
    SpotLightGpuData spotLights[SFT_MAX_SPOT_LIGHTS];
    PointLightGpuData pointLights[SFT_MAX_POINT_LIGHTS];
    ShadowViewGpuData shadowViews[SFT_MAX_SHADOW_VIEWS];
};

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<AtmosphereGpuData> atmosphereData;
Texture2D<float4> gbufferAlbedo;
Texture2D<float2> gbufferNormal;
Texture2D<float4> gbufferMaterial;
Texture2D<float4> gbufferEmissive;
Texture2D<float> gbufferDepth;
Texture2D<float4> spectralEffect;
Texture2D<float> shadowAtlas;
Texture2D<float4> transmittanceLut;
Texture2D<float4> multiScatteringLut;
Texture2D<float4> skyViewLut;
SamplerState gbufferSampler;
SamplerState shadowDepthSampler;
SamplerComparisonState shadowCompareSampler;
SamplerState atmosphereSampler;

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = fullscreenTriangleUv(vertexId);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

float linearShadowDepth(float depth, ShadowViewGpuData view) {
    float nearPlane = view.depthParams.x;
    float farPlane = view.depthParams.y;
    if (view.depthParams.z < 0.5) {
        return nearPlane + depth * (farPlane - nearPlane);
    }
    return nearPlane * farPlane / max(farPlane - depth * (farPlane - nearPlane), SFT_EPSILON);
}

float2 clampAtlasUv(float2 atlasUv, ShadowViewGpuData view) {
    float2 halfTexel = lightingData.shadowParams.xx * 0.5;
    float2 minimum = view.atlasScaleBias.zw + halfTexel;
    float2 maximum = view.atlasScaleBias.zw + view.atlasScaleBias.xy - halfTexel;
    return clamp(atlasUv, minimum, maximum);
}

float2 receiverPlaneDepthGradient(float2 localUv, float receiverDepth,
                                  float3 biasedPosition, float3 worldDx, float3 worldDy,
                                  ShadowViewGpuData view) {
    // Convert screen-space position derivatives to d(light depth)/d(light UV). Each PCF/PCSS tap
    // can then compare against the receiver plane at that tap instead of reusing the center depth.
    // This removes the need for a large global bias on sloped, finely tessellated receivers.
    float4 clipDx = mul(float4(biasedPosition + worldDx, 1.0), view.viewProjection);
    float4 clipDy = mul(float4(biasedPosition + worldDy, 1.0), view.viewProjection);
    if (abs(clipDx.w) <= SFT_EPSILON || abs(clipDy.w) <= SFT_EPSILON) {
        return float2(0.0);
    }
    float3 ndcDx = clipDx.xyz / clipDx.w;
    float3 ndcDy = clipDy.xyz / clipDy.w;
    float2 uvDx = ndcToUv(ndcDx.xy) - localUv;
    float2 uvDy = ndcToUv(ndcDy.xy) - localUv;
    float depthDx = ndcDx.z - receiverDepth;
    float depthDy = ndcDy.z - receiverDepth;
    float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(determinant) <= 1.0e-10) {
        return float2(0.0);
    }
    float2 gradient = float2(
        (depthDx * uvDy.y - uvDx.y * depthDy) / determinant,
        (uvDx.x * depthDy - depthDx * uvDy.x) / determinant);
    // Degenerate quads at silhouettes can produce an unbounded plane. A one-depth-range-per-UV
    // slope is already steeper than useful filtering and is a safe conservative cap.
    return clamp(gradient, float2(-1.0), float2(1.0));
}

float receiverDepthAtTap(float receiverDepth, float2 receiverGradient, float2 offsetLocal) {
    return saturate(receiverDepth + dot(receiverGradient, offsetLocal));
}


float filteredShadowVisibility(ShadowViewGpuData view, float2 atlasUv, float receiverDepth,
                               float2 receiverGradient, float filterRadiusLocal, float depthEpsilon) {
    float visible = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint i = 0; i < SFT_PCSS_FILTER_SAMPLES; ++i) {
        float2 diskSample = SFT_FILTER_DISK[i];
        float2 offsetLocal = diskSample * filterRadiusLocal;
        float2 sampleUv = clampAtlasUv(atlasUv + offsetLocal * view.atlasScaleBias.xy, view);
        float tapReceiverDepth = receiverDepthAtTap(receiverDepth, receiverGradient, offsetLocal);
        float compareDepth = saturate(tapReceiverDepth - depthEpsilon);
        float weight = 1.0 / (1.0 + 2.0 * dot(diskSample, diskSample));
        visible += shadowAtlas.SampleCmpLevelZero(shadowCompareSampler, sampleUv, compareDepth) * weight;
        totalWeight += weight;
    }
    return visible / max(totalWeight, SFT_EPSILON);
}

float shadowVisibility(uint viewIndex, float3 worldPosition, float3 normal, float3 lightVector,
                       float3 worldDx, float3 worldDy) {
    if (viewIndex >= (uint)lightingData.counts.z) {
        return 1.0;
    }
    ShadowViewGpuData view = lightingData.shadowViews[viewIndex];

    // Project once without bias so a configured offset can be expressed in shadow texels. A fixed
    // world-space offset is necessarily too large for small/near geometry or too small far away.
    float4 unbiasedClip = mul(float4(worldPosition, 1.0), view.viewProjection);
    if (unbiasedClip.w <= SFT_EPSILON) {
        return 1.0;
    }
    float3 unbiasedNdc = unbiasedClip.xyz / unbiasedClip.w;
    float2 unbiasedLocalUv = ndcToUv(unbiasedNdc.xy);
    if (unbiasedNdc.z <= 0.0 || unbiasedNdc.z >= 1.0 ||
        any(unbiasedLocalUv <= 0.0) || any(unbiasedLocalUv >= 1.0)) {
        return 1.0;
    }
    float unbiasedLinear = linearShadowDepth(unbiasedNdc.z, view);
    float tileResolution = max(view.filterParams.y, 1.0);
    float worldTexel = view.filterParams.x / tileResolution;
    if (view.depthParams.z > 0.5) {
        worldTexel *= max(unbiasedLinear, view.depthParams.x);
    }
    float3 receiverNormal = safeNormalizeOr(cross(worldDx, worldDy), normal);
    if (dot(receiverNormal, normal) < 0.0) {
        receiverNormal = -receiverNormal;
    }
    float nDotL = saturate(dot(receiverNormal, lightVector));
    float grazing = sqrt(saturate(1.0 - nDotL * nDotL));
    float normalOffsetTexels = min(lightingData.shadowParams.y * grazing, 2.0);
    float3 biasedPosition = worldPosition + receiverNormal * (worldTexel * normalOffsetTexels);
    float4 clip = mul(float4(biasedPosition, 1.0), view.viewProjection);
    if (clip.w <= SFT_EPSILON) {
        return 1.0;
    }
    float3 ndc = clip.xyz / clip.w;
    float2 localUv = ndcToUv(ndc.xy);
    if (ndc.z <= 0.0 || ndc.z >= 1.0 || any(localUv <= 0.0) || any(localUv >= 1.0)) {
        return 1.0;
    }
    float2 atlasUv = localUv * view.atlasScaleBias.xy + view.atlasScaleBias.zw;
    float receiverDepth = ndc.z;
    float receiverLinear = linearShadowDepth(receiverDepth, view);
    float2 receiverGradient = receiverPlaneDepthGradient(
        localUv, receiverDepth, biasedPosition, worldDx, worldDy, view);
    float minimumRadiusLocal = 0.85 / tileResolution;
    float maxFilterRadiusLocal = view.filterParams.z > SFT_EPSILON
                                     ? max(view.filterParams.z, minimumRadiusLocal)
                                     : 0.10;
    float maxSearchRadiusLocal = view.filterParams.w > SFT_EPSILON
                                     ? max(view.filterParams.w, minimumRadiusLocal)
                                     : 0.075;
    const float depthEpsilon = 2.0e-6;

    if (lightingData.shadowParams.z < 0.5 || view.depthParams.w <= SFT_EPSILON) {
        return filteredShadowVisibility(
            view, atlasUv, receiverDepth, receiverGradient, minimumRadiusLocal, depthEpsilon);
    }

    float searchRadiusLocal = max(1.5 / tileResolution, minimumRadiusLocal);
    if (lightingData.shadowParams.z > 0.5) {
        float nearPlane = view.depthParams.x;
        if (view.depthParams.z < 0.5) {
            searchRadiusLocal = max(searchRadiusLocal,
                view.depthParams.w * max(receiverLinear - nearPlane, 0.0));
        } else {
            searchRadiusLocal = max(searchRadiusLocal,
                view.depthParams.w * max(receiverLinear - nearPlane, 0.0) /
                max(nearPlane * receiverLinear, SFT_EPSILON));
        }
        searchRadiusLocal = min(searchRadiusLocal, maxSearchRadiusLocal);
    }

    float blockerDepthSum = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (uint i = 0; i < SFT_PCSS_BLOCKER_SAMPLES; ++i) {
        float2 offsetLocal = SFT_BLOCKER_DISK[i] * searchRadiusLocal;
        float2 sampleUv = clampAtlasUv(atlasUv + offsetLocal * view.atlasScaleBias.xy, view);
        float sampleDepth = shadowAtlas.SampleLevel(shadowDepthSampler, sampleUv, 0.0);
        float tapReceiverDepth = receiverDepthAtTap(receiverDepth, receiverGradient, offsetLocal);
        if (sampleDepth + depthEpsilon < tapReceiverDepth) {
            float blockerLinear = linearShadowDepth(sampleDepth, view);
            blockerDepthSum += blockerLinear;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5) {
        return 1.0;
    }

    float blockerLinear = blockerDepthSum / blockerCount;
    float filterRadiusLocal = minimumRadiusLocal;
    if (lightingData.shadowParams.z > 0.5) {
        if (view.depthParams.z < 0.5) {
            filterRadiusLocal = max(filterRadiusLocal,
                view.depthParams.w * max(receiverLinear - blockerLinear, 0.0));
        } else {
            filterRadiusLocal = max(filterRadiusLocal,
                view.depthParams.w * max(receiverLinear - blockerLinear, 0.0) /
                max(blockerLinear * receiverLinear, SFT_EPSILON));
        }
        filterRadiusLocal = min(filterRadiusLocal, maxFilterRadiusLocal);
    }

    return filteredShadowVisibility(
        view, atlasUv, receiverDepth, receiverGradient, filterRadiusLocal, depthEpsilon);
}

// Short same-frame refinement for details that are too small to survive the raster-map footprint
// (feet, thin stems, cables). This deliberately uses fixed, spatially deterministic samples: visible
// quality does not depend on TAA/history. A ray that leaves the screen or reaches sky depth remains
// lit, so unavailable screen data can never invent an off-screen occluder.
float screenSpaceSunContactShadow(float3 worldPosition, float3 sunVector) {
    if (lightingData.contactShadowParams.w < 0.5 ||
        lightingData.contactShadowParams.x <= SFT_EPSILON) {
        return 1.0;
    }

    uint stepCount = min((uint)lightingData.contactShadowParams.z, 12u);
    float stepLength = lightingData.contactShadowParams.x / max((float)stepCount, 1.0);
    float thickness = lightingData.contactShadowParams.y;
    // Screen-space contact shadows are only valid as a short-range refinement. If the depth buffer
    // reports geometry that is far from the marched point, that is almost always an unrelated
    // foreground/background overlap rather than a true contact occluder.
    float maxWorldSeparation = max(thickness * 2.0, stepLength * 2.0);
    float maxViewSeparation = max(thickness * 4.0, stepLength * 2.0);
    [unroll]
    for (uint i = 0; i < 12u; ++i) {
        if (i >= stepCount) break;
        float3 candidate = worldPosition + sunVector * (stepLength * (float)(i + 1u));
        float4 candidateClip = mul(float4(candidate, 1.0), lightingData.viewProjection);
        if (candidateClip.w <= SFT_EPSILON) return 1.0;
        float3 candidateNdc = candidateClip.xyz / candidateClip.w;
        float2 candidateUv = ndcToUv(candidateNdc.xy);
        if (candidateNdc.z <= 0.0 || candidateNdc.z >= 1.0 ||
            any(candidateUv <= 0.0) || any(candidateUv >= 1.0)) {
            return 1.0;
        }

        float sceneDepth = gbufferDepth.SampleLevel(gbufferSampler, candidateUv, 0.0);
        if (sceneDepth >= 0.999999) continue;
        float4 sceneWorldH = mul(float4(uvToNdc(candidateUv), sceneDepth, 1.0),
                                 lightingData.inverseViewProjection);
        if (abs(sceneWorldH.w) <= SFT_EPSILON) continue;
        float3 sceneWorld = sceneWorldH.xyz / sceneWorldH.w;
        float candidateViewDepth = -mul(float4(candidate, 1.0), lightingData.view).z;
        float sceneViewDepth = -mul(float4(sceneWorld, 1.0), lightingData.view).z;
        float viewSeparation = candidateViewDepth - sceneViewDepth;
        if (viewSeparation <= thickness) continue;

        float worldSeparation = length(sceneWorld - candidate);
        if (viewSeparation > maxViewSeparation || worldSeparation > maxWorldSeparation) {
            continue;
        }

        return 0.0;
    }
    return 1.0;
}

float directionalShadow(float3 worldPosition, float3 normal, float3 lightVector,
                        float3 worldDx, float3 worldDy, float viewDepth) {
    uint cascadeCount = (uint)lightingData.sun.cascadeParams.x;
    if (lightingData.sun.radianceShadow.w < 0.5 || cascadeCount == 0) {
        return 1.0;
    }
    uint cascade = 0;
    [unroll]
    for (uint i = 0; i < 3; ++i) {
        cascade += (i + 1 < cascadeCount && viewDepth > lightingData.sun.cascadeSplits[i]) ? 1u : 0u;
    }
    float cascadeEnd = lightingData.sun.cascadeSplits[cascade];
    if (viewDepth > cascadeEnd) {
        return 1.0;
    }
    uint firstView = (uint)lightingData.sun.cascadeParams.z;
    float visibility = shadowVisibility(firstView + cascade, worldPosition, normal, lightVector,
                                        worldDx, worldDy);

    float cascadeStart = cascade == 0 ? lightingData.cameraPositionNear.w
                                      : lightingData.sun.cascadeSplits[cascade - 1];
    float blendWidth = max((cascadeEnd - cascadeStart) * lightingData.sun.cascadeParams.y, SFT_EPSILON);
    float blend = saturate((viewDepth - (cascadeEnd - blendWidth)) / blendWidth);
    if (cascade + 1 < cascadeCount) {
        float nextVisibility = shadowVisibility(firstView + cascade + 1, worldPosition, normal,
                                                lightVector, worldDx, worldDy);
        visibility = lerp(visibility, nextVisibility, blend);
    } else {
        visibility = lerp(visibility, 1.0, blend);
    }
    return visibility;
}

uint pointShadowFace(float3 fromLight) {
    float3 a = abs(fromLight);
    if (a.x >= a.y && a.x >= a.z) return fromLight.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.x && a.y >= a.z) return fromLight.y >= 0.0 ? 2u : 3u;
    return fromLight.z >= 0.0 ? 4u : 5u;
}

float distributionGGXLocal(float nDotH, float alphaRoughness) {
    float a2 = alphaRoughness * alphaRoughness;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 * SFT_INV_PI / max(d * d, SFT_EPSILON);
}

float geometrySmithCorrelated(float nDotV, float nDotL, float alphaRoughness) {
    float a2 = alphaRoughness * alphaRoughness;
    float gv = nDotL * sqrt(max(nDotV * nDotV * (1.0 - a2) + a2, SFT_EPSILON));
    float gl = nDotV * sqrt(max(nDotL * nDotL * (1.0 - a2) + a2, SFT_EPSILON));
    return 0.5 / max(gv + gl, SFT_EPSILON);
}

float3 evaluateDirect(float3 albedo, float metallic, float perceptualRoughness,
                      float alphaRoughness, float dielectricF0,
                      float3 normal, float3 viewVector, float3 lightVector, float3 radiance) {
    float nDotL = saturate(dot(normal, lightVector));
    float nDotV = saturate(dot(normal, viewVector));
    if (nDotL <= 0.0 || nDotV <= 0.0) return float3(0.0);
    float3 halfVector = safeNormalizeOr(lightVector + viewVector, normal);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewVector, halfVector));
    float3 f0 = lerp(float3(dielectricF0), albedo, metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow5(1.0 - vDotH);
    float distribution = distributionGGXLocal(nDotH, alphaRoughness);
    float visibility = geometrySmithCorrelated(nDotV, nDotL, alphaRoughness);
    float3 specular = fresnel * distribution * visibility;
    float3 diffuseBrdf = evaluateEonDiffuse(
        albedo, perceptualRoughness, normal, lightVector, viewVector);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * diffuseBrdf;
    return (diffuse + specular) * radiance * nDotL;
}

float punctualAttenuation(float distanceToLight, float range) {
    float ratio = distanceToLight / max(range, SFT_EPSILON);
    float window = saturate(1.0 - pow4(ratio));
    return square(window) / max(distanceToLight * distanceToLight, 0.01);
}

float3 reconstructViewPosition(float2 uv, float depth) {
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 worldH = mul(clip, lightingData.inverseViewProjection);
    float3 world = worldH.xyz / max(abs(worldH.w), SFT_EPSILON) * signNotZero(worldH.w);
    return mul(float4(world, 1.0), lightingData.view).xyz;
}

// World-space view ray direction for a background pixel (nothing in the gbuffer to derive it from —
// unproject the far clip plane instead and point away from the camera through it).
float3 reconstructWorldRayDirection(float2 uv) {
    float4 clip = float4(uvToNdc(uv), 1.0, 1.0);
    float4 worldH = mul(clip, lightingData.inverseViewProjection);
    float3 worldFar = worldH.xyz / max(abs(worldH.w), SFT_EPSILON) * signNotZero(worldH.w);
    return safeNormalizeOr(worldFar - lightingData.cameraPositionNear.xyz, float3(0.0, 1.0, 0.0));
}

// Jimenez et al. GTAO horizon integration. This is deliberately spatial-only: the per-pixel
// rotation is a stable interleaved pattern and enough horizon samples are taken that no temporal
// history is needed. Distance attenuation and a minimum separation reject depth discontinuities
// and sub-pixel slivers that would otherwise become leaking dark halos.
float groundTruthAmbientOcclusion(float2 uv, float depth, float3 worldNormal, float2 pixelPosition) {
    uint quality = (uint)lightingData.viewportParams.w;
    if (quality == 0u) {
        return 1.0;
    }

    float3 viewPosition = reconstructViewPosition(uv, depth);
    float viewDepth = max(abs(viewPosition.z), lightingData.cameraPositionNear.w);
    float3 viewDirection = safeNormalizeOr(-viewPosition, float3(0.0, 0.0, 1.0));
    float3 viewNormal = safeNormalizeOr(
        mul(float4(worldNormal, 0.0), lightingData.view).xyz, float3(0.0, 0.0, 1.0));

    float radius = lightingData.gtaoParams.x;
    float screenRadius = 0.5 * radius * lightingData.viewportParams.z / viewDepth;
    float radiusClamp = min(1.0, 0.25 / max(screenRadius, SFT_EPSILON));
    screenRadius *= radiusClamp;
    float effectiveRadius = radius * radiusClamp;

    uint directionCount = min(quality * 2u + 2u, 8u);
    uint stepCount = min(quality + 1u, 5u);
    float stableNoise = frac(dot(floor(pixelPosition), float2(0.06711056, 0.00583715)));
    float visibility = 0.0;
    float visibilityNormalization = 0.0;

    [loop]
    for (uint directionIndex = 0u; directionIndex < directionCount; ++directionIndex) {
        float phi = SFT_PI * (float(directionIndex) + stableNoise) / float(directionCount);
        float2 omega = float2(cos(phi), sin(phi));
        float3 sliceDirection = float3(omega, 0.0);
        float3 sliceAxis = safeNormalizeOr(cross(sliceDirection, viewDirection), float3(0.0, 1.0, 0.0));
        float3 projectedNormal = viewNormal - sliceAxis * dot(viewNormal, sliceAxis);
        float projectedLength = length(projectedNormal);
        if (projectedLength <= SFT_EPSILON) {
            continue;
        }
        projectedNormal /= projectedLength;

        float signedNormal = dot(cross(viewDirection, projectedNormal), sliceAxis) < 0.0 ? -1.0 : 1.0;
        float cosNormal = saturate(dot(projectedNormal, viewDirection));
        float normalAngle = signedNormal * acos(cosNormal);
        float sliceVisibility = 0.0;

        [unroll]
        for (uint side = 0u; side < 2u; ++side) {
            float sideSign = side == 0u ? -1.0 : 1.0;
            float horizonCos = -1.0;
            [loop]
            for (uint step = 0u; step < 5u; ++step) {
                if (step >= stepCount) {
                    break;
                }
                float stepFraction = (float(step) + 0.5 + stableNoise * 0.35) / float(stepCount);
                // Contact occlusion has the most fine geometry, so distribute more taps nearby.
                stepFraction *= stepFraction;
                float2 sampleUv = uv + sideSign * omega * screenRadius * stepFraction;
                if (any(sampleUv <= 0.0) || any(sampleUv >= 1.0)) {
                    continue;
                }
                float sampleDepth = gbufferDepth.SampleLevel(gbufferSampler, sampleUv, 0.0);
                if (sampleDepth >= 0.999999) {
                    continue;
                }
                float3 samplePosition = reconstructViewPosition(sampleUv, sampleDepth);
                float3 delta = samplePosition - viewPosition;
                float distanceToSample = length(delta);
                float minimumSeparation = max(
                    effectiveRadius * 0.0025,
                    viewDepth * max(lightingData.viewportParams.x, lightingData.viewportParams.y));
                if (distanceToSample <= minimumSeparation || distanceToSample >= effectiveRadius) {
                    continue;
                }
                float falloffStart = effectiveRadius * lightingData.gtaoParams.y;
                float distanceWeight = saturate(
                    (effectiveRadius - distanceToSample) /
                    max(effectiveRadius - falloffStart, SFT_EPSILON));
                float candidate = dot(delta / distanceToSample, viewDirection);
                float weightedCandidate = lerp(-1.0, candidate, distanceWeight);
                if (weightedCandidate >= horizonCos) {
                    horizonCos = weightedCandidate;
                } else if (candidate < horizonCos) {
                    // A lone close-depth texel is often foliage or a silhouette discontinuity, not
                    // an infinitely thick blocker. Let later lower samples relax the horizon by a
                    // bounded amount instead of turning that texel into a broad dark leak.
                    float thinCompensation =
                        0.1 * saturate(lightingData.gtaoParams.z / max(effectiveRadius, SFT_EPSILON));
                    horizonCos = max(weightedCandidate, horizonCos - thinCompensation);
                }
            }

            float horizonAngle = normalAngle + clamp(
                sideSign * acos(clamp(horizonCos, -1.0, 1.0)) - normalAngle,
                -0.5 * SFT_PI, 0.5 * SFT_PI);
            sliceVisibility +=
                (cosNormal + 2.0 * horizonAngle * sin(normalAngle) -
                 cos(2.0 * horizonAngle - normalAngle)) * 0.25;
        }

        visibility += sliceVisibility * projectedLength;
        visibilityNormalization +=
            (normalAngle * sin(normalAngle) + cosNormal) * projectedLength;
    }

    float ao = saturate(visibility / max(visibilityNormalization, SFT_EPSILON));
    return pow(ao, lightingData.gtaoParams.w);
}

float3 gtaoMultiBounce(float visibility, float3 albedo) {
    // Fitted near-field color bounce from the GTAO paper keeps colored surfaces from losing all
    // indirect energy when the scalar visibility term is low.
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    return max(float3(visibility), ((a * visibility + b) * visibility + c) * visibility);
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float depth = gbufferDepth.SampleLevel(gbufferSampler, input.uv, 0.0);
    AtmosphereParams atmosphere = unpackAtmosphereParams(atmosphereData);
    float3 planetOrigin = atmosphereData.cameraPositionPlanetSpace.xyz;
    float3 sunDirToward = safeNormalizeOr(atmosphereData.sunDirectionAngularRadius.xyz, float3(0.0, 1.0, 0.0));
    float atmosphereViewHeight = max(length(planetOrigin), atmosphere.planetRadiusMeters + 1.0);

    if (depth >= 0.999999) {
        float3 rayDir = reconstructWorldRayDirection(input.uv);
        float tGroundUnused;
        bool rayIntersectsGround = distanceToGround(atmosphere, planetOrigin, rayDir, tGroundUnused);
        float2 skyUv = skyViewLutUv(atmosphere, rayDir, float3(0.0, 1.0, 0.0), sunDirToward,
                                    atmosphereViewHeight, rayIntersectsGround, SFT_SKY_VIEW_LUT_SIZE);
        float3 sky = skyViewLut.SampleLevel(atmosphereSampler, skyUv, 0.0).rgb;
        float3 upVector = planetOrigin * safeRcp(atmosphereViewHeight);
        float3 transmittanceToSpace = sampleTransmittanceLut(transmittanceLut, atmosphereSampler, atmosphere,
                                                              atmosphereViewHeight, dot(upVector, rayDir));
        sky += sunDiskLuminance(rayDir, sunDirToward, atmosphereData.sunDirectionAngularRadius.w,
                                atmosphereData.sunIlluminance.rgb, transmittanceToSpace);
        return float4(sky, 1.0);
    }

    float4 clip = float4(uvToNdc(input.uv), depth, 1.0);
    float4 worldH = mul(clip, lightingData.inverseViewProjection);
    float3 worldPosition = worldH.xyz / max(abs(worldH.w), SFT_EPSILON) * signNotZero(worldH.w);
    float4 albedoSample = gbufferAlbedo.SampleLevel(gbufferSampler, input.uv, 0.0);
    float3 albedo = albedoSample.rgb;
    float3 normal = decodeOctahedralNormal(gbufferNormal.SampleLevel(gbufferSampler, input.uv, 0.0));
    float4 material = gbufferMaterial.SampleLevel(gbufferSampler, input.uv, 0.0);
    float perceptualRoughness = saturate(material.x);
    float roughness = max(perceptualRoughness * perceptualRoughness, SFT_MIN_PBR_ROUGHNESS);
    float metallic = saturate(material.y);
    float ambientOcclusion = saturate(material.z);
    uint spectralMode = (uint)round(lightingData.spectralParams.x);
    float4 tracedEffect = spectralEffect.SampleLevel(gbufferSampler, input.uv, 0.0);
    float dielectricF0 = saturate(material.w);
    float3 viewVector = safeNormalizeOr(lightingData.cameraPositionNear.xyz - worldPosition, normal);
    float3 worldDx = ddx(worldPosition);
    float3 worldDy = ddy(worldPosition);
    float viewDepth = abs(mul(float4(worldPosition, 1.0), lightingData.view).z);
    float2 pixelPosition = input.position.xy;
    ambientOcclusion *= spectralMode == 3u
        ? saturate(tracedEffect.r)
        : groundTruthAmbientOcclusion(input.uv, depth, normal, pixelPosition);

    float3 hdr = albedo * lightingData.ambientRadianceExposure.rgb *
                 gtaoMultiBounce(ambientOcclusion, albedo);
    float3 sunVector = -safeNormalizeOr(lightingData.sun.directionAngularRadius.xyz, float3(0.0, -1.0, 0.0));
    float3 sunVisibility = float3(directionalShadow(worldPosition, normal, sunVector, worldDx, worldDy,
                                                    viewDepth));
    sunVisibility *= screenSpaceSunContactShadow(worldPosition, sunVector);
    if (spectralMode == 1u) sunVisibility = saturate(tracedEffect.rrr);
    if (spectralMode == 4u) sunVisibility = max(tracedEffect.rgb, 0.0);
    hdr += evaluateDirect(albedo, metallic, perceptualRoughness, roughness, dielectricF0, normal, viewVector,
                          sunVector, lightingData.sun.radianceShadow.rgb) * sunVisibility;
    if (spectralMode == 2u) {
        float3 specularTint = lerp(float3(dielectricF0), albedo, metallic);
        hdr += max(tracedEffect.rgb, 0.0) * specularTint * (1.0 - saturate(roughness));
    }

    uint spotCount = min((uint)lightingData.counts.x, SFT_MAX_SPOT_LIGHTS);
    [loop]
    for (uint i = 0; i < spotCount; ++i) {
        SpotLightGpuData light = lightingData.spotLights[i];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight >= light.positionRange.w) continue;
        float3 lightVector = toLight / max(distanceToLight, SFT_EPSILON);
        float coneCos = dot(safeNormalizeOr(light.directionOuterCos.xyz, float3(0.0, -1.0, 0.0)), -lightVector);
        float cone = smoothstep(light.directionOuterCos.w, light.radianceInnerCos.w, coneCos);
        if (cone <= 0.0) continue;
        float visibility = 1.0;
        int shadowView = (int)light.shadowParams.x;
        if (shadowView >= 0) {
            visibility = shadowVisibility((uint)shadowView, worldPosition, normal, lightVector,
                                          worldDx, worldDy);
        }
        float3 radiance = light.radianceInnerCos.rgb * punctualAttenuation(distanceToLight, light.positionRange.w) * cone;
        hdr += evaluateDirect(albedo, metallic, perceptualRoughness, roughness, dielectricF0, normal, viewVector,
                              lightVector, radiance) * visibility;
    }

    uint pointCount = min((uint)lightingData.counts.y, SFT_MAX_POINT_LIGHTS);
    [loop]
    for (uint i = 0; i < pointCount; ++i) {
        PointLightGpuData light = lightingData.pointLights[i];
        float3 toLight = light.positionRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        if (distanceToLight >= light.positionRange.w) continue;
        float3 lightVector = toLight / max(distanceToLight, SFT_EPSILON);
        float visibility = 1.0;
        int firstShadowView = (int)light.shadowParams.x;
        if (firstShadowView >= 0) {
            uint face = pointShadowFace(worldPosition - light.positionRange.xyz);
            visibility = shadowVisibility((uint)firstShadowView + face, worldPosition, normal,
                                          lightVector, worldDx, worldDy);
        }
        float3 radiance = light.radianceSourceRadius.rgb * punctualAttenuation(distanceToLight, light.positionRange.w);
        hdr += evaluateDirect(albedo, metallic, perceptualRoughness, roughness, dielectricF0, normal, viewVector,
                              lightVector, radiance) * visibility;
    }

    hdr *= lightingData.ambientRadianceExposure.w;
    // Emissive is unlit self-illumination (glTF semantics) — added after exposure, not scaled by
    // it, so an emissive surface (screen, lava, neon) reads the same brightness regardless of the
    // ambient exposure the rest of the scene is dialed to, and bypasses BRDF/shadow/AO entirely.
    float3 emissive = gbufferEmissive.SampleLevel(gbufferSampler, input.uv, 0.0).rgb;
    hdr += emissive;

    // Aerial perspective: blend the lit result with in-scattered atmosphere light between the camera
    // and this surface, using the exact same integrator the sky-view LUT was baked with (just a much
    // shorter raymarch, since scene geometry is close relative to the atmosphere's scale height).
    float3 toPixel = worldPosition - lightingData.cameraPositionNear.xyz;
    float pixelDistance = length(toPixel);
    float3 aerialDir = pixelDistance > SFT_EPSILON ? toPixel / pixelDistance : float3(0.0, 0.0, 1.0);
    // includeGroundBounce=false: this ray already hit real, fully-lit scene geometry at
    // pixelDistance — the mathematical "ground" plane sits at very nearly the same height as a real
    // floor/table for any downward-looking ray, so leaving it enabled here double-added a flat grey
    // ground-albedo term on top of already-lit surfaces almost everywhere (visible as a fog/haze).
    ScatteringResult aerial = integrateScatteredLuminance(
        atmosphere, planetOrigin, aerialDir, sunDirToward, atmosphereData.sunIlluminance.rgb,
        pixelDistance, 8u, transmittanceLut, atmosphereSampler, multiScatteringLut, atmosphereSampler,
        /*useMultiScattering*/ true, /*includeGroundBounce*/ false);
    hdr = hdr * aerial.transmittance + aerial.luminance;

    return float4(max(hdr, 0.0), albedoSample.a);
}
