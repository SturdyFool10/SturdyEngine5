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
static const uint SFT_PCF_SAMPLES = 16;
static const float SFT_MIN_PBR_ROUGHNESS = 0.045;

// Golden-angle increment used to build Vogel disks.
static const float SFT_GOLDEN_ANGLE = 2.39996322972865332;

// Shadow debug visualizations; mirrors `Engine::ShadowDebugView`.
static const uint SFT_SHADOW_DEBUG_NONE = 0;
static const uint SFT_SHADOW_DEBUG_CASCADE_INDEX = 1;
static const uint SFT_SHADOW_DEBUG_CASCADE_FADE = 2;
static const uint SFT_SHADOW_DEBUG_TEXEL_GRID = 3;
static const uint SFT_SHADOW_DEBUG_SHADOW_UV = 4;
static const uint SFT_SHADOW_DEBUG_RECEIVER_DEPTH = 5;
static const uint SFT_SHADOW_DEBUG_ATLAS_DEPTH = 6;
static const uint SFT_SHADOW_DEBUG_DEPTH_DELTA = 7;
static const uint SFT_SHADOW_DEBUG_NORMAL_BIAS = 8;
static const uint SFT_SHADOW_DEBUG_RECEIVER_PLANE_GRADIENT = 9;
static const uint SFT_SHADOW_DEBUG_HARD_COMPARISON = 10;
static const uint SFT_SHADOW_DEBUG_PCF = 11;
static const uint SFT_SHADOW_DEBUG_DIRECTIONAL_CSM = 12;
static const uint SFT_SHADOW_DEBUG_CONTACT = 13;
static const uint SFT_SHADOW_DEBUG_COMBINED_SUN_VISIBILITY = 14;
static const uint SFT_RENDER_DEBUG_GBUFFER_DEPTH = 15;
static const uint SFT_RENDER_DEBUG_WORLD_POSITION = 16;
static const uint SFT_RENDER_DEBUG_GBUFFER_NORMAL = 17;
static const uint SFT_RENDER_DEBUG_GBUFFER_ALBEDO = 18;
static const uint SFT_RENDER_DEBUG_GBUFFER_ROUGHNESS = 19;
static const uint SFT_RENDER_DEBUG_GBUFFER_METALLIC = 20;
static const uint SFT_RENDER_DEBUG_MATERIAL_AO = 21;
static const uint SFT_RENDER_DEBUG_AMBIENT_LIGHTING = 22;
static const uint SFT_RENDER_DEBUG_SUN_N_DOT_L = 23;
static const uint SFT_RENDER_DEBUG_UNSHADOWED_SUN_LIGHTING = 24;
static const uint SFT_RENDER_DEBUG_AMBIENT_OCCLUSION = 25;

// Vogel (sunflower) disk point `index` of `count`, rotated by `rotation` radians.
//
// A Vogel disk is near-uniform for any tap count and has no axis-aligned structure, so unlike a
// hand-authored fixed disk it does not print a recognizable static pattern into penumbrae. The
// per-pixel rotation supplied by the caller breaks up the remaining regularity.
float2 vogelDiskSample(uint index, uint count, float rotation) {
    float radius = sqrt((float(index) + 0.5) / float(count));
    float theta = float(index) * SFT_GOLDEN_ANGLE + rotation;
    return float2(cos(theta), sin(theta)) * radius;
}

// Interleaved gradient noise. Cheap, well-distributed, and exactly reproducible for a given input.
float interleavedGradientNoise(float2 position) {
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

// Rotation angle for the PCF/PCSS disk of one receiver.
//
// Deliberately derived from the receiver's position *in shadow space*, not from its screen
// position. Cascades are texel-snapped and world-locked, so a fixed world point keeps the same
// shadow-space coordinate as the camera moves; the dither pattern therefore sticks to the surface
// instead of crawling across it. That matters because there is no temporal filter downstream to
// clean up a screen-anchored pattern: it has to be stable in a single frame and under motion.
float shadowFilterRotation(float2 localUv, float tileResolution) {
    return interleavedGradientNoise(localUv * tileResolution * 4.0) * SFT_PI * 2.0;
}

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
    // View-space distance at which each cascade ends.
    float4 cascadeSplits;
    // View-space distance at which each cascade begins cross-fading into the next one. Computed on
    // the CPU from the cascade's own extent, not from a fixed percentage of the split distance.
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
    // (max intensity, fade-out view distance, unused, unused)
    float4 contactShadowParamsExtra;
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
// Spot/point shadow atlas.
Texture2D<float> shadowAtlas;
// Dedicated directional cascade atlas. Kept separate from `shadowAtlas` so cascade resolution is
// decided by configuration rather than by whatever the punctual atlas packer has left over.
Texture2D<float> directionalShadowAtlas;
Texture2D<float4> transmittanceLut;
Texture2D<float4> multiScatteringLut;
Texture2D<float4> skyViewLut;
Texture2D<float4> surfelIrradiance;
// Screen-space ambient occlusion from the spatial-only XeGTAO passes (Shaders/gtao_main.slang),
// already denoised and intensity-scaled. A 1x1 white dummy is bound when AO is disabled, so this
// shader never needs to branch on the feature toggle.
Texture2D<float> gtaoAmbientOcclusion;
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

// Hard guarantee that a tap never leaves this view's own atlas tile.
//
// The CPU already insets each cascade's receiver region by a guard band wide enough for the maximum
// filter radius plus the maximum normal offset, so in correct operation this clamp is inert. It
// stays as a backstop: reading one texel of a neighbouring allocation produces a completely wrong
// occlusion result, which is far worse than a clamped tap at the tile border.
//
// Half-texel size is derived per view from the tile's own scale/resolution, so it is correct for
// both atlases even though they have different (and non-square) dimensions.
float2 clampAtlasUv(float2 atlasUv, ShadowViewGpuData view) {
    float2 halfTexel = 0.5 * view.atlasScaleBias.xy / max(view.filterParams.y, 1.0);
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


// Rotated-Vogel PCF over hardware depth comparison.
//
// `filterRadiusLocal` is in tile-local UV (0..1 across the cascade tile), so a caller that derives
// it from texel counts gets the same perceived softness at every cascade resolution. Each tap
// compares against the receiver *plane* evaluated at that tap rather than against the centre depth,
// which is what keeps sloped receivers from needing a large constant bias.
float filteredShadowVisibility(Texture2D<float> atlas, ShadowViewGpuData view, float2 atlasUv,
                               float receiverDepth, float2 receiverGradient, float filterRadiusLocal,
                               float rotation, float depthEpsilon) {
    float visible = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint i = 0; i < SFT_PCF_SAMPLES; ++i) {
        float2 diskSample = vogelDiskSample(i, SFT_PCF_SAMPLES, rotation);
        float2 offsetLocal = diskSample * filterRadiusLocal;
        float2 sampleUv = clampAtlasUv(atlasUv + offsetLocal * view.atlasScaleBias.xy, view);
        float tapReceiverDepth = receiverDepthAtTap(receiverDepth, receiverGradient, offsetLocal);
        float compareDepth = saturate(tapReceiverDepth - depthEpsilon);
        // Mild radial falloff: a box-weighted disk has a visible hard outer edge, and the Vogel
        // distribution already places more taps near the rim than a uniform kernel wants.
        float weight = 1.0 / (1.0 + 2.0 * dot(diskSample, diskSample));
        visible += atlas.SampleCmpLevelZero(shadowCompareSampler, sampleUv, compareDepth) * weight;
        totalWeight += weight;
    }
    return visible / max(totalWeight, SFT_EPSILON);
}

// Result of projecting a receiver into one shadow view, shared by the resolve and the debug views.
struct ShadowReceiver {
    // True when the receiver lies inside the view's frustum and can be evaluated at all.
    bool valid;
    // Tile-local UV (0..1 across the tile) of the normal-offset receiver position.
    float2 localUv;
    // Atlas UV of the same point.
    float2 atlasUv;
    // Post-projection depth of the receiver, matching the atlas' depth encoding.
    float depth;
    // Light-space (linear) depth of the receiver.
    float linearDepth;
    // d(depth)/d(tile-local UV) of the receiver plane.
    float2 gradient;
    // World-space size of one shadow texel at this receiver.
    float worldTexel;
};

// Projects a receiver into a shadow view and applies the normal-offset bias.
//
// Bias responsibilities are deliberately split, because they solve different problems:
//   * Constant and slope-scaled *depth* bias live in the rasterizer state of the shadow pass, where
//     they act on the caster and cost nothing here.
//   * The *normal* offset applied below moves the sample point off the receiver surface, which is
//     what defeats self-shadowing from the finite shadow-map footprint at grazing angles.
//   * The *receiver-plane* gradient (`gradient`) corrects each individual filter tap for the slope
//     of the receiver, so the filter radius does not have to be paid for with extra depth bias.
//   * The filter footprint itself is paid for by the guard band the CPU reserves inside the tile.
// Making any one of these compensate for the others is what produces either acne or peter-panning.
ShadowReceiver projectShadowReceiver(ShadowViewGpuData view, float3 worldPosition, float3 normal,
                                     float3 lightVector, float3 worldDx, float3 worldDy) {
    ShadowReceiver receiver;
    receiver.valid = false;
    receiver.localUv = float2(0.0);
    receiver.atlasUv = float2(0.0);
    receiver.depth = 0.0;
    receiver.linearDepth = 0.0;
    receiver.gradient = float2(0.0);
    receiver.worldTexel = 0.0;

    // Project once without bias so the offset below can be expressed in shadow texels. A fixed
    // world-space offset is necessarily too large for small/near geometry or too small far away.
    float4 unbiasedClip = mul(float4(worldPosition, 1.0), view.viewProjection);
    if (unbiasedClip.w <= SFT_EPSILON) {
        return receiver;
    }
    float3 unbiasedNdc = unbiasedClip.xyz / unbiasedClip.w;
    float2 unbiasedLocalUv = ndcToUv(unbiasedNdc.xy);
    if (unbiasedNdc.z <= 0.0 || unbiasedNdc.z >= 1.0 ||
        any(unbiasedLocalUv <= 0.0) || any(unbiasedLocalUv >= 1.0)) {
        return receiver;
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
    // receiverNormal is a flat, per-triangle geometric normal (cross of screen-space position
    // derivatives), not the smooth shading normal -- correct for biasing against the actual
    // rasterized triangle plane, but it means nDotL itself is noisy/discontinuous across triangle
    // edges on curved or tessellated geometry. A curve with an unbounded derivative as nDotL -> 0
    // (e.g. 1/nDotL) amplifies that per-triangle noise into visibly different bias amounts between
    // adjacent triangles -- a faceted/triangulated shadow-edge pattern (confirmed empirically: this
    // is exactly what a 1/nDotL version of this formula produced on Sponza's arches). Keep the
    // sqrt(1-nDotL^2) (sine of the angle from the light) curve, which has a bounded derivative.
    //
    // The 3.0 texel cap is mirrored by kMaxNormalOffsetTexels in RendererShadow.cpp, which sizes the
    // tile guard band. The two must move together or the guard stops being a guarantee.
    float grazing = sqrt(saturate(1.0 - nDotL * nDotL));
    float normalOffsetTexels = min(lightingData.shadowParams.y * (1.0 + grazing * 2.5), 3.0);
    float3 biasedPosition = worldPosition + receiverNormal * (worldTexel * normalOffsetTexels);

    float4 clip = mul(float4(biasedPosition, 1.0), view.viewProjection);
    if (clip.w <= SFT_EPSILON) {
        return receiver;
    }
    float3 ndc = clip.xyz / clip.w;
    float2 localUv = ndcToUv(ndc.xy);
    if (ndc.z <= 0.0 || ndc.z >= 1.0 || any(localUv <= 0.0) || any(localUv >= 1.0)) {
        return receiver;
    }

    receiver.valid = true;
    receiver.localUv = localUv;
    receiver.atlasUv = localUv * view.atlasScaleBias.xy + view.atlasScaleBias.zw;
    receiver.depth = ndc.z;
    receiver.linearDepth = linearShadowDepth(ndc.z, view);
    receiver.gradient = receiverPlaneDepthGradient(localUv, ndc.z, biasedPosition, worldDx, worldDy, view);
    receiver.worldTexel = worldTexel;
    return receiver;
}

// Evaluates sun/spot/point occlusion for one receiver against one shadow view.
//
// `atlas` selects the physical atlas the view lives in; the two are never mixed within a call.
float shadowVisibility(Texture2D<float> atlas, uint viewIndex, float3 worldPosition, float3 normal,
                       float3 lightVector, float3 worldDx, float3 worldDy) {
    if (viewIndex >= (uint)lightingData.counts.z) {
        return 1.0;
    }
    ShadowViewGpuData view = lightingData.shadowViews[viewIndex];
    ShadowReceiver receiver = projectShadowReceiver(view, worldPosition, normal, lightVector,
                                                    worldDx, worldDy);
    if (!receiver.valid) {
        return 1.0;
    }

    float tileResolution = max(view.filterParams.y, 1.0);
    // Base PCF radius is authored in shadow texels and converted here, so changing a cascade's
    // resolution changes its sharpness but not its perceived filter width.
    float baseRadiusLocal = max(lightingData.shadowParams.x, 0.5) / tileResolution;
    // The CPU sized the tile guard band from exactly this cap, so no tap can escape the tile.
    float maxFilterRadiusLocal = view.filterParams.z > SFT_EPSILON
                                     ? max(view.filterParams.z, baseRadiusLocal)
                                     : 0.10;
    float maxSearchRadiusLocal = view.filterParams.w > SFT_EPSILON
                                     ? max(view.filterParams.w, baseRadiusLocal)
                                     : 0.075;
    // Small constant offset in post-projection depth units, purely to absorb the quantization of
    // the depth format. Acne is handled by the raster depth/slope bias and the receiver plane, not
    // by inflating this.
    const float depthEpsilon = 2.0e-6;
    float rotation = shadowFilterRotation(receiver.localUv, tileResolution);

    if (lightingData.shadowParams.z < 0.5 || view.depthParams.w <= SFT_EPSILON) {
        return filteredShadowVisibility(atlas, view, receiver.atlasUv, receiver.depth,
                                        receiver.gradient, baseRadiusLocal, rotation, depthEpsilon);
    }

    // --- PCSS ---------------------------------------------------------------------------------
    // Optional contact hardening. It refines an already-correct hard-shadow result; it is never
    // relied on to hide cascade fitting or resolution problems, and its footprint is clamped so a
    // far cascade cannot dissolve into an unbounded blur.
    float nearPlane = view.depthParams.x;
    float searchRadiusLocal = baseRadiusLocal;
    if (view.depthParams.z < 0.5) {
        searchRadiusLocal = max(searchRadiusLocal,
            view.depthParams.w * max(receiver.linearDepth - nearPlane, 0.0));
    } else {
        searchRadiusLocal = max(searchRadiusLocal,
            view.depthParams.w * max(receiver.linearDepth - nearPlane, 0.0) /
            max(nearPlane * receiver.linearDepth, SFT_EPSILON));
    }
    searchRadiusLocal = min(searchRadiusLocal, maxSearchRadiusLocal);

    float blockerDepthSum = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (uint i = 0; i < SFT_PCSS_BLOCKER_SAMPLES; ++i) {
        float2 offsetLocal = vogelDiskSample(i, SFT_PCSS_BLOCKER_SAMPLES, rotation) * searchRadiusLocal;
        float2 sampleUv = clampAtlasUv(receiver.atlasUv + offsetLocal * view.atlasScaleBias.xy, view);
        float sampleDepth = atlas.SampleLevel(shadowDepthSampler, sampleUv, 0.0);
        float tapReceiverDepth = receiverDepthAtTap(receiver.depth, receiver.gradient, offsetLocal);
        if (sampleDepth + depthEpsilon < tapReceiverDepth) {
            blockerDepthSum += linearShadowDepth(sampleDepth, view);
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5) {
        return 1.0;
    }

    float blockerLinear = blockerDepthSum / blockerCount;
    float filterRadiusLocal = baseRadiusLocal;
    if (view.depthParams.z < 0.5) {
        filterRadiusLocal = max(filterRadiusLocal,
            view.depthParams.w * max(receiver.linearDepth - blockerLinear, 0.0));
    } else {
        filterRadiusLocal = max(filterRadiusLocal,
            view.depthParams.w * max(receiver.linearDepth - blockerLinear, 0.0) /
            max(blockerLinear * receiver.linearDepth, SFT_EPSILON));
    }
    filterRadiusLocal = min(filterRadiusLocal, maxFilterRadiusLocal);

    return filteredShadowVisibility(atlas, view, receiver.atlasUv, receiver.depth, receiver.gradient,
                                    filterRadiusLocal, rotation, depthEpsilon);
}

// Approximate world-space size of one screen pixel at a given view depth.
//
// Derived from the vertical projection scale and the render height, both already published in
// `viewportParams`. Used to push ray origins off the surface by a distance that is meaningful at
// any range, instead of by a fixed world constant that is wrong everywhere but one depth.
float worldSizePerPixel(float viewDepth) {
    float projectionScale = max(lightingData.viewportParams.z, SFT_EPSILON);
    return viewDepth * 2.0 * lightingData.viewportParams.y / projectionScale;
}

// Deterministic, world-anchored dither in [0, 1) for offsetting a ray start.
//
// Anchored to the receiver's world position rather than to its pixel coordinate, so the pattern is
// locked to the surface: as the camera moves it stays put instead of boiling. That property is
// mandatory here because nothing downstream is temporal -- there is no history buffer to resolve a
// screen-anchored pattern, so it has to be stable in a single frame and under motion.
float contactShadowDither(float3 worldPosition) {
    // frac() first keeps the hash argument small and well-conditioned regardless of how far the
    // scene origin is from the receiver.
    float3 p = frac(worldPosition * 91.7);
    return frac(sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

// Fades toward zero as a sample approaches the edge of the screen.
//
// The marched ray leaves the depth buffer at the screen border. Cutting the effect off there
// produces a hard seam along the frame edge, so the term is faded out over a narrow margin instead.
float screenBorderFade(float2 uv) {
    float2 distanceToEdge = min(uv, 1.0 - uv);
    return saturate(min(distanceToEdge.x, distanceToEdge.y) / 0.05);
}

// Confidence that the depth-buffer surface under a marched ray point is a genuine contact occluder.
//
// `separation` is how far the ray point sits *behind* that surface in view space. Too small and the
// ray has not actually passed behind anything; too large and the depth buffer is showing unrelated
// geometry (typically a foreground object near a silhouette) rather than a thin occluder the ray
// went behind. Both limits are smooth rather than binary: hard limits are precisely what turns a
// silhouette into a hard-edged black halo, because adjacent pixels land on opposite sides of the
// threshold.
//
// @return 0 when the sample cannot be an occluder, rising to 1 well inside the thickness window.
float contactOccluderConfidence(float separation, float thickness, float maxSeparation) {
    float entering = smoothstep(thickness, thickness * 2.0 + SFT_EPSILON, separation);
    float leaving = 1.0 - smoothstep(maxSeparation * 0.5, maxSeparation, separation);
    return entering * leaving;
}

// Short same-frame refinement for contact detail that is too small to survive the raster-map
// footprint (feet, thin stems, cables).
//
// This *supplements* the cascades and is deliberately unable to stand in for them: its strength is
// capped by `contactShadowParamsExtra.x`, it fades out entirely past
// `contactShadowParamsExtra.y`, and it only ever looks `contactShadowParams.x` world units along
// the sun vector. If this term is doing visible heavy lifting, the cascade fit is the thing to fix.
//
// Everything here is single-frame by construction: fixed step count, world-anchored dither, no
// history. A ray that leaves the screen or reaches sky depth fades out rather than reporting
// occlusion, so absent screen data can never invent an off-screen occluder.
//
// @param worldPosition Receiver world position.
// @param normal Receiver shading normal.
// @param sunVector Unit vector pointing from the receiver towards the sun.
// @param worldDx Screen-space world-position derivative in x.
// @param worldDy Screen-space world-position derivative in y.
// @param viewDepth Positive view-space distance to the receiver.
//
// @return Sun visibility in [1 - intensity, 1]. Never returns a hard 0 unless intensity is 1.
float screenSpaceSunContactShadow(float3 worldPosition, float3 normal, float3 sunVector,
                                  float3 worldDx, float3 worldDy, float viewDepth) {
    if (lightingData.contactShadowParams.w < 0.5 ||
        lightingData.contactShadowParams.x <= SFT_EPSILON) {
        return 1.0;
    }

    // Bias and reject against the flat per-triangle geometric normal, not the interpolated shading
    // normal: the artifact being defended against is the march re-detecting the rasterized surface
    // the ray started on, and that surface is the triangle plane.
    float3 geometricNormal = safeNormalizeOr(cross(worldDx, worldDy), normal);
    if (dot(geometricNormal, normal) < 0.0) {
        geometricNormal = -geometricNormal;
    }

    // A surface already turned away from the sun is shadowed by the N.L term. Marching it can only
    // manufacture false occlusion at silhouettes, so skip it outright.
    float nDotL = dot(geometricNormal, sunVector);
    if (nDotL <= 0.0) {
        return 1.0;
    }

    // Near-field only. Past the fade distance one shadow texel is already finer than the detail
    // this march recovers, so the cascades are strictly the better answer and this is switched off.
    float fadeDistance = max(lightingData.contactShadowParamsExtra.y, SFT_EPSILON);
    float distanceFade = 1.0 - saturate((viewDepth - fadeDistance * 0.75) / (fadeDistance * 0.25));
    if (distanceFade <= 0.0) {
        return 1.0;
    }

    uint stepCount = clamp((uint)lightingData.contactShadowParams.z, 2u, 12u);
    float rayLength = lightingData.contactShadowParams.x;
    float stepLength = rayLength / (float)stepCount;
    float thickness = lightingData.contactShadowParams.y;
    float maxSeparation = max(thickness * 4.0, stepLength * 2.0);

    // Start slightly off the surface along its own normal. Without this the first sample can read
    // the receiver's own depth and self-occlude, which is a direct source of solid black patches.
    float originOffset = worldSizePerPixel(viewDepth) * 2.0;
    float3 origin = worldPosition + geometricNormal * originOffset;
    // Sub-step offset. A march with a fixed step pattern quantizes into hard bands along the sun's
    // screen-space direction; offsetting each receiver's ray start decorrelates neighbouring pixels
    // so the quantization becomes a gradient instead of a stair.
    float dither = contactShadowDither(worldPosition);

    float occlusion = 0.0;
    [unroll]
    for (uint i = 0; i < 12u; ++i) {
        if (i >= stepCount) break;
        float t = ((float)i + dither) * stepLength;
        float3 candidate = origin + sunVector * t;

        float4 candidateClip = mul(float4(candidate, 1.0), lightingData.viewProjection);
        if (candidateClip.w <= SFT_EPSILON) break;
        float3 candidateNdc = candidateClip.xyz / candidateClip.w;
        if (candidateNdc.z <= 0.0 || candidateNdc.z >= 1.0) break;
        float2 candidateUv = ndcToUv(candidateNdc.xy);
        if (any(candidateUv <= 0.0) || any(candidateUv >= 1.0)) break;

        float sceneDepth = gbufferDepth.SampleLevel(gbufferSampler, candidateUv, 0.0);
        // Sky: nothing occludes here.
        if (sceneDepth >= 0.999999) continue;
        float4 sceneWorldH = mul(float4(uvToNdc(candidateUv), sceneDepth, 1.0),
                                 lightingData.inverseViewProjection);
        if (abs(sceneWorldH.w) <= SFT_EPSILON) continue;
        float3 sceneWorld = sceneWorldH.xyz / sceneWorldH.w;

        float candidateViewDepth = -mul(float4(candidate, 1.0), lightingData.view).z;
        float sceneViewDepth = -mul(float4(sceneWorld, 1.0), lightingData.view).z;
        float confidence = contactOccluderConfidence(candidateViewDepth - sceneViewDepth,
                                                     thickness, maxSeparation);
        if (confidence <= 0.0) continue;

        // Receiver-plane rejection.
        //
        // Because the ray starts off the surface and climbs away from it at nDotL per unit, at
        // parameter t it sits `rayHeight` above the receiver's plane. Anything that genuinely
        // occludes it must therefore also reach that height. A depth-buffer sample lying on the
        // receiver's own plane (height ~ 0) cannot occlude a ray already above it -- that reading is
        // the march re-detecting the surface it started on, which is what produces the speckled
        // false occlusion wherever the sun rakes along a large flat surface.
        //
        // Crucially this is a *height* test, not a proximity test: an object standing on the
        // receiver (a pole on a floor) has surface points at the ray's own height, so its contact
        // shadow -- the whole point of the effect -- survives, while the floor it stands on does not
        // shadow itself.
        float rayHeight = originOffset + t * nDotL;
        float sampleHeight = dot(sceneWorld - worldPosition, geometricNormal);

        // Depth reconstruction and bilinear depth sampling cannot distinguish a sub-pixel offset
        // from the original triangle exactly. Reject an explicit, footprint-scaled slab around the
        // receiver plane before applying the height test below. This is particularly important at
        // grazing light angles: rayHeight then grows very slowly, so a ratio-only test can mistake
        // reconstruction noise from the receiver itself for a second, dithered shadow.
        float receiverPlaneTolerance = max(originOffset * 2.0, worldSizePerPixel(viewDepth) * 2.0);
        if (abs(sampleHeight) <= receiverPlaneTolerance) continue;

        // Measure only height beyond the receiver-plane slab. A real raised occluder still has to
        // reach the marched ray; the receiver's own plane has already been rejected above.
        float occluderHeight = max(sampleHeight - receiverPlaneTolerance, 0.0);
        float requiredHeight = max(rayHeight - receiverPlaneTolerance, receiverPlaneTolerance);
        confidence *= smoothstep(0.15, 0.5, occluderHeight / requiredHeight);
        if (confidence <= 0.0) continue;

        // Occluders found near the receiver are what this effect is for; ones at the far end of the
        // ray are increasingly likely to be unrelated geometry, so their contribution tapers off.
        float rangeFade = 1.0 - smoothstep(0.5, 1.0, t / max(rayLength, SFT_EPSILON));
        occlusion = max(occlusion, confidence * rangeFade * screenBorderFade(candidateUv));
    }

    float intensity = saturate(lightingData.contactShadowParamsExtra.x);
    return saturate(1.0 - occlusion * intensity * distanceFade);
}

// Cascade selection plus the cross-fade weight for a given view-space distance.
//
// `cascade` is the authoritative cascade for this pixel; `blend` is how far the pixel has advanced
// into the fade band towards `cascade + 1` (0 = fully this cascade, 1 = fully the next). The band is
// expressed as explicit view-space distances computed on the CPU from the cascade's own extent, so
// its width tracks the cascade geometry rather than a fixed percentage.
void selectShadowCascade(float viewDepth, out uint cascade, out float blend) {
    uint cascadeCount = (uint)lightingData.sun.cascadeParams.x;
    cascade = 0;
    [unroll]
    for (uint i = 0; i < 3; ++i) {
        cascade += (i + 1 < cascadeCount && viewDepth > lightingData.sun.cascadeSplits[i]) ? 1u : 0u;
    }
    float cascadeEnd = lightingData.sun.cascadeSplits[cascade];
    float fadeStart = lightingData.sun.cascadeFadeStarts[cascade];
    blend = saturate((viewDepth - fadeStart) / max(cascadeEnd - fadeStart, SFT_EPSILON));
}

float directionalShadow(float3 worldPosition, float3 normal, float3 lightVector,
                        float3 worldDx, float3 worldDy, float viewDepth) {
    uint cascadeCount = (uint)lightingData.sun.cascadeParams.x;
    if (lightingData.sun.radianceShadow.w < 0.5 || cascadeCount == 0) {
        return 1.0;
    }
    uint cascade;
    float blend;
    selectShadowCascade(viewDepth, cascade, blend);
    if (viewDepth > lightingData.sun.cascadeSplits[cascade]) {
        return 1.0;
    }

    uint firstView = (uint)lightingData.sun.cascadeParams.z;
    float visibility = shadowVisibility(directionalShadowAtlas, firstView + cascade, worldPosition,
                                        normal, lightVector, worldDx, worldDy);
    // Only pixels actually inside the fade band pay for a second cascade evaluation. Without this
    // the transition would silently double the shadow cost of every pixel in the cascade.
    if (blend <= 0.0) {
        return visibility;
    }
    if (cascade + 1 < cascadeCount) {
        float nextVisibility = shadowVisibility(directionalShadowAtlas, firstView + cascade + 1,
                                                worldPosition, normal, lightVector, worldDx, worldDy);
        return lerp(visibility, nextVisibility, blend);
    }
    // Beyond the last cascade there is no shadow data, so fade out rather than cut off.
    return lerp(visibility, 1.0, blend);
}

// Distinct per-cascade debug colors.
float3 cascadeDebugColor(uint cascade) {
    if (cascade == 0) return float3(1.0, 0.25, 0.25);
    if (cascade == 1) return float3(0.25, 1.0, 0.35);
    if (cascade == 2) return float3(0.3, 0.5, 1.0);
    return float3(1.0, 0.9, 0.25);
}

// Single-frame shadow debug visualizations.
//
// Returns false when the pixel is outside every cascade, so the caller can leave it shaded normally
// and the cascade footprint is directly readable on screen.
bool directionalShadowDebug(uint mode, float3 worldPosition, float3 normal, float3 lightVector,
                            float3 worldDx, float3 worldDy, float viewDepth, out float3 debugColor) {
    debugColor = float3(0.0);
    uint cascadeCount = (uint)lightingData.sun.cascadeParams.x;
    if (mode == SFT_SHADOW_DEBUG_NONE || lightingData.sun.radianceShadow.w < 0.5 || cascadeCount == 0) {
        return false;
    }
    uint cascade;
    float blend;
    selectShadowCascade(viewDepth, cascade, blend);
    if (viewDepth > lightingData.sun.cascadeSplits[cascade]) {
        return false;
    }

    uint viewIndex = (uint)lightingData.sun.cascadeParams.z + cascade;
    if (viewIndex >= (uint)lightingData.counts.z) {
        return false;
    }
    ShadowViewGpuData view = lightingData.shadowViews[viewIndex];
    ShadowReceiver receiver = projectShadowReceiver(view, worldPosition, normal, lightVector,
                                                    worldDx, worldDy);
    if (!receiver.valid) {
        // Inside the cascade's depth range but outside its projection: the coverage invariant has
        // been violated. Flag it loudly rather than blending it away.
        debugColor = float3(1.0, 0.0, 1.0);
        return true;
    }

    float tileResolution = max(view.filterParams.y, 1.0);
    float baseRadiusLocal = max(lightingData.shadowParams.x, 0.5) / tileResolution;
    const float depthEpsilon = 2.0e-6;
    float comparisonDepth = saturate(receiver.depth - depthEpsilon);
    float atlasDepth = directionalShadowAtlas.SampleLevel(shadowDepthSampler, receiver.atlasUv, 0.0);

    // Stages before filtering. They intentionally do not call the complete shadow path, so they
    // remain useful when diagnosing whether an artifact starts in projection/bias or in filtering.
    if (mode == SFT_SHADOW_DEBUG_SHADOW_UV) {
        debugColor = float3(receiver.atlasUv, 0.25);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_RECEIVER_DEPTH) {
        debugColor = receiver.depth.xxx;
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_ATLAS_DEPTH) {
        debugColor = atlasDepth.xxx;
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_DEPTH_DELTA) {
        // Neutral grey means the reconstructed receiver and stored caster agree. Red is occluded
        // (receiver behind caster); blue is lit. Scale by local derivatives to expose sub-texel
        // disagreement without requiring a scene-specific depth-range constant.
        float delta = receiver.depth - atlasDepth;
        float normalizedDelta = saturate(abs(delta) / max(fwidth(receiver.depth) * 8.0, 1.0e-5));
        debugColor = delta >= 0.0 ? float3(0.5 + 0.5 * normalizedDelta, 0.5, 0.5 - 0.5 * normalizedDelta)
                                  : float3(0.5 - 0.5 * normalizedDelta, 0.5, 0.5 + 0.5 * normalizedDelta);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_NORMAL_BIAS) {
        float4 unbiasedClip = mul(float4(worldPosition, 1.0), view.viewProjection);
        float2 unbiasedUv = ndcToUv(unbiasedClip.xy / max(unbiasedClip.w, SFT_EPSILON));
        float offsetTexels = length((receiver.localUv - unbiasedUv) * tileResolution);
        debugColor = float3(saturate(offsetTexels / 3.0), saturate(abs(receiver.depth -
                            unbiasedClip.z / max(unbiasedClip.w, SFT_EPSILON)) * 8192.0), 0.0);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_RECEIVER_PLANE_GRADIENT) {
        debugColor = float3(saturate(receiver.gradient * 8.0 + 0.5), 0.5);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_HARD_COMPARISON) {
        float hardVisibility = directionalShadowAtlas.SampleCmpLevelZero(
            shadowCompareSampler, receiver.atlasUv, comparisonDepth);
        debugColor = hardVisibility.xxx;
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_PCF) {
        float pcfVisibility = filteredShadowVisibility(
            directionalShadowAtlas, view, receiver.atlasUv, receiver.depth, receiver.gradient,
            baseRadiusLocal, shadowFilterRotation(receiver.localUv, tileResolution), depthEpsilon);
        debugColor = pcfVisibility.xxx;
        return true;
    }

    float visibility = shadowVisibility(directionalShadowAtlas, viewIndex, worldPosition, normal,
                                        lightVector, worldDx, worldDy);
    float3 tint = cascadeDebugColor(cascade);

    if (mode == SFT_SHADOW_DEBUG_CASCADE_INDEX) {
        debugColor = tint * (0.25 + 0.75 * visibility);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_CASCADE_FADE) {
        float3 nextTint = cascade + 1 < cascadeCount ? cascadeDebugColor(cascade + 1) : float3(1.0);
        // White banding marks the fade region so its width is directly measurable on screen.
        debugColor = lerp(tint, nextTint, blend) * (0.25 + 0.75 * visibility) +
                     float3(blend * (1.0 - blend) * 2.0);
        return true;
    }
    if (mode == SFT_SHADOW_DEBUG_TEXEL_GRID) {
        // World-locked grid: if the cascade were swimming or crawling, these lines would slide over
        // static geometry as the camera moves.
        float2 texel = receiver.localUv * tileResolution;
        float2 texelWidth = max(fwidth(texel), float2(1.0e-6));
        float2 distanceToLine = abs(frac(texel) - 0.5) / texelWidth;
        float line = 1.0 - saturate(min(distanceToLine.x, distanceToLine.y) - 0.5);
        float resolvable = 1.0 - smoothstep(0.5, 1.0, max(texelWidth.x, texelWidth.y));
        debugColor = lerp(tint * (0.25 + 0.75 * visibility), float3(1.0), line * resolvable * 0.85);
        return true;
    }
    // SFT_SHADOW_DEBUG_DIRECTIONAL_CSM: the selected cascade before its fade is visualized above;
    // this mode deliberately evaluates the full cascade-selection and blend stage.
    debugColor = directionalShadow(worldPosition, normal, lightVector, worldDx, worldDy, viewDepth).xxx;
    return true;
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

// World-space view ray direction for a background pixel (nothing in the gbuffer to derive it from —
// unproject the far clip plane instead and point away from the camera through it).
float3 reconstructWorldRayDirection(float2 uv) {
    float4 clip = float4(uvToNdc(uv), 1.0, 1.0);
    float4 worldH = mul(clip, lightingData.inverseViewProjection);
    float3 worldFar = worldH.xyz / max(abs(worldH.w), SFT_EPSILON) * signNotZero(worldH.w);
    return safeNormalizeOr(worldFar - lightingData.cameraPositionNear.xyz, float3(0.0, 1.0, 0.0));
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
    float materialAmbientOcclusion = saturate(material.z);
    uint spectralMode = (uint)round(lightingData.spectralParams.x);
    float4 tracedEffect = spectralEffect.SampleLevel(gbufferSampler, input.uv, 0.0);
    float dielectricF0 = saturate(material.w);
    uint shadowDebugMode = (uint)round(lightingData.spectralParams.w);

    // G-buffer stage diagnostics. These run before AO/shadow evaluation and continue to work when
    // the shadow feature is disabled, which makes them the first stop for a purported shadow that
    // is actually encoded in material data or geometry reconstruction.
    if (shadowDebugMode == SFT_RENDER_DEBUG_GBUFFER_DEPTH) return float4(depth.xxx, 1.0);
    if (shadowDebugMode == SFT_RENDER_DEBUG_WORLD_POSITION) {
        return float4(frac(abs(worldPosition) * 0.1), 1.0);
    }
    if (shadowDebugMode == SFT_RENDER_DEBUG_GBUFFER_NORMAL) {
        return float4(normal * 0.5 + 0.5, 1.0);
    }
    if (shadowDebugMode == SFT_RENDER_DEBUG_GBUFFER_ALBEDO) return float4(albedo, 1.0);
    if (shadowDebugMode == SFT_RENDER_DEBUG_GBUFFER_ROUGHNESS) return float4(perceptualRoughness.xxx, 1.0);
    if (shadowDebugMode == SFT_RENDER_DEBUG_GBUFFER_METALLIC) return float4(metallic.xxx, 1.0);
    if (shadowDebugMode == SFT_RENDER_DEBUG_MATERIAL_AO) return float4(materialAmbientOcclusion.xxx, 1.0);

    float3 viewVector = safeNormalizeOr(lightingData.cameraPositionNear.xyz - worldPosition, normal);
    float3 worldDx = ddx(worldPosition);
    float3 worldDy = ddy(worldPosition);
    float viewDepth = abs(mul(float4(worldPosition, 1.0), lightingData.view).z);
    // Ambient occlusion is a visibility term for *indirect* irradiance, so it multiplies the
    // environment ambient and the surfel GI bounce and nothing else. Applying it to the whole HDR
    // buffer - crushing sun lighting, punctual lights, and specular highlights alike - is what makes
    // AO read as a smeared grey decal rather than as contact shading, and it is not what the term
    // means physically: a surface fully lit by a visible light source is not dark just because its
    // hemisphere is partly blocked. Emissive is added later and bypasses this entirely.
    float screenSpaceAmbientOcclusion = saturate(
        gtaoAmbientOcclusion.SampleLevel(gbufferSampler, input.uv, 0.0));
    // Ray-traced ambient-occlusion visibility, when the spectral AO integrator produced one.
    // `viewportParams.w` carries the ambient-occlusion feature toggle. That integrator resolves the
    // same visibility this pass's screen-space term approximates, so it replaces it rather than
    // multiplying with it (and the screen-space passes are skipped for that render mode anyway).
    if (spectralMode == 3u && lightingData.viewportParams.w > 0.5) {
        screenSpaceAmbientOcclusion = saturate(tracedEffect.r);
    }
    float indirectOcclusion = materialAmbientOcclusion * screenSpaceAmbientOcclusion;
    if (shadowDebugMode == SFT_RENDER_DEBUG_AMBIENT_OCCLUSION) {
        return float4(screenSpaceAmbientOcclusion.xxx, 1.0);
    }

    float3 hdr = albedo * lightingData.ambientRadianceExposure.rgb * indirectOcclusion;
    if (lightingData.spectralParams.y > 0.5) {
        float3 surfelGi = surfelIrradiance.SampleLevel(gbufferSampler, input.uv, 0.0).rgb *
                           lightingData.spectralParams.z;
        hdr += albedo * surfelGi * indirectOcclusion;
    }
    float3 sunVector = -safeNormalizeOr(lightingData.sun.directionAngularRadius.xyz, float3(0.0, -1.0, 0.0));
    if (shadowDebugMode == SFT_RENDER_DEBUG_AMBIENT_LIGHTING) return float4(hdr, 1.0);
    if (shadowDebugMode == SFT_RENDER_DEBUG_SUN_N_DOT_L) {
        return float4(saturate(dot(normal, sunVector)).xxx, 1.0);
    }
    if (shadowDebugMode == SFT_RENDER_DEBUG_UNSHADOWED_SUN_LIGHTING) {
        float3 unshadowedSun = evaluateDirect(
            albedo, metallic, perceptualRoughness, roughness, dielectricF0, normal, viewVector,
            sunVector, lightingData.sun.radianceShadow.rgb);
        return float4(unshadowedSun, 1.0);
    }
    if (shadowDebugMode == SFT_SHADOW_DEBUG_CONTACT ||
        shadowDebugMode == SFT_SHADOW_DEBUG_COMBINED_SUN_VISIBILITY) {
        // Inspect the screen-space supplement independently, or combined with the complete CSM
        // resolve exactly as normal lighting uses it.
        float contact = screenSpaceSunContactShadow(worldPosition, normal, sunVector,
                                                    worldDx, worldDy, viewDepth);
        if (shadowDebugMode == SFT_SHADOW_DEBUG_CONTACT) {
            return float4(float3(contact), 1.0);
        }
        float csm = directionalShadow(worldPosition, normal, sunVector, worldDx, worldDy, viewDepth);
        return float4(float3(csm * contact), 1.0);
    }
    if (shadowDebugMode != SFT_SHADOW_DEBUG_NONE) {
        float3 debugColor;
        if (directionalShadowDebug(shadowDebugMode, worldPosition, normal, sunVector, worldDx,
                                   worldDy, viewDepth, debugColor)) {
            return float4(debugColor, 1.0);
        }
    }
    float3 sunVisibility = float3(directionalShadow(worldPosition, normal, sunVector, worldDx, worldDy,
                                                    viewDepth));
    sunVisibility *= screenSpaceSunContactShadow(worldPosition, normal, sunVector, worldDx,
                                                 worldDy, viewDepth);
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
            visibility = shadowVisibility(shadowAtlas, (uint)shadowView, worldPosition, normal,
                                          lightVector, worldDx, worldDy);
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
            visibility = shadowVisibility(shadowAtlas, (uint)firstShadowView + face, worldPosition,
                                          normal, lightVector, worldDx, worldDy);
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
