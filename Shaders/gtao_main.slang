// GTAO pass 2: the horizon search itself.
//
// This is Algorithm 1 of Jimenez et al. 2016 ("Practical Realtime Strategies for Accurate Indirect
// Occlusion") in the form Intel's XeGTAO ships it, with the temporal half removed. For each pixel a
// small number of azimuthal *slices* is stepped outward in screen space; each step reconstructs the
// sampled surface's view-space position and records how high above the horizon it sits, and the
// per-slice maximum horizon angles are folded into the analytic visibility integral of the paper
// (section 3.3) rather than the older "count occluded samples" obscurance estimator.
//
// Deviations from stock XeGTAO, all forced by this renderer having no temporal reconstruction:
//
//  * The noise is a pure function of pixel coordinate (see gtaoSpatialNoise) - no frame index, no
//    animated blue noise, no reprojection. A static camera renders a bit-identical AO buffer.
//  * Off-screen taps are faded out toward the hemisphere horizon instead of being clamped to the
//    border texel. Clamping makes the edge texel act as an occluder for everything beyond it, which
//    shows up as a dark band down the side of the screen at grazing angles - XeGTAO gets away with
//    it because its temporal pass has history from when the geometry was on screen.
//
// Output is deliberately *raw*: the AO term and the depth-edge information the 5x5 denoiser needs.
// Nothing here is applied to lighting; see gtao_denoise.slang and deferred_shadow_lighting.slang.
import sturdy_common;
import gtao_common;

ConstantBuffer<GtaoConstants> gtao;
// Linear view-space depth with the 5-level mip chain built by gtao_prefilter_depth.slang.
Texture2D<float> viewDepthPyramid;
// Octahedral-encoded world-space normal, exactly as gbuffer_geometry.slang wrote it.
Texture2D<float2> gbufferNormal;
SamplerState gtaoPointSampler;
RWTexture2D<float> ambientOcclusionOut;
RWTexture2D<float4> edgesOut;

// Per-axis depth discontinuity strength toward each of the four neighbours, in [0, 1]
// (1 = continuous surface, 0 = hard discontinuity the denoiser must not blur across).
//
// The slope-adjusted comparison is what keeps a steeply sloped but perfectly continuous floor from
// registering as an edge: a plane seen at a grazing angle has a large raw depth delta, but its
// second difference is ~0, so comparing against the locally extrapolated plane cancels it out.
float4 calculateEdges(float centerZ, float leftZ, float rightZ, float topZ, float bottomZ) {
    float4 edges = float4(leftZ, rightZ, topZ, bottomZ) - centerZ;
    float slopeLR = (edges.y - edges.x) * 0.5;
    float slopeTB = (edges.w - edges.z) * 0.5;
    float4 slopeAdjusted = edges + float4(slopeLR, -slopeLR, slopeTB, -slopeTB);
    edges = min(abs(edges), abs(slopeAdjusted));
    // Tolerance scales with view depth because depth precision and texel footprint both do.
    return saturate(1.25 - edges / (centerZ * 0.011));
}

// Attenuation for a tap that has left (or is about to leave) the screen.
//
// Returns 1 well inside the viewport and falls smoothly to 0 at the border. Callers fold this into
// the tap's falloff weight, which lerps the sampled horizon cosine back toward the low horizon -
// i.e. an off-screen tap contributes *no occlusion* rather than an invented one.
float screenEdgeFade(float2 uv, GtaoConstants gtao) {
    float2 fadeUv = gtao.depthLinearizeEdgeIntensity.zz * gtao.viewportSizePixelSize.zw;
    float2 distanceToBorder = min(uv, 1.0 - uv);
    float2 fade = saturate(distanceToBorder / max(fadeUv, float2(SFT_EPSILON)));
    return min(fade.x, fade.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gtaoMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = uint2(gtao.viewportSizePixelSize.xy);
    if (any(dispatchThreadId.xy >= extent)) return;

    int2 pixel = int2(dispatchThreadId.xy);
    int2 maxPixel = int2(extent) - int2(1, 1);
    float2 viewportPixelSize = gtao.viewportSizePixelSize.zw;
    float2 uv = (float2(pixel) + 0.5) * viewportPixelSize;

    float viewZ = viewDepthPyramid.Load(int3(pixel, 0));
    float leftZ = viewDepthPyramid.Load(int3(clamp(pixel + int2(-1, 0), int2(0, 0), maxPixel), 0));
    float rightZ = viewDepthPyramid.Load(int3(clamp(pixel + int2(1, 0), int2(0, 0), maxPixel), 0));
    float topZ = viewDepthPyramid.Load(int3(clamp(pixel + int2(0, -1), int2(0, 0), maxPixel), 0));
    float bottomZ = viewDepthPyramid.Load(int3(clamp(pixel + int2(0, 1), int2(0, 0), maxPixel), 0));
    float4 edges = calculateEdges(viewZ, leftZ, rightZ, topZ, bottomZ);
    edgesOut[pixel] = edges;

    // Sky/background: nothing is occluded, and running the search would only waste bandwidth on a
    // result the lighting pass discards anyway.
    float farPlane = gtao.depthLinearizeEdgeIntensity.x /
                     max(gtao.depthLinearizeEdgeIntensity.y - 1.0, SFT_EPSILON);
    if (viewZ >= farPlane * 0.999) {
        ambientOcclusionOut[pixel] = 1.0;
        return;
    }

    float3 worldNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    float3 viewspaceNormal = gtaoViewSpaceNormal(worldNormal, gtao);

    float3 pixelCenterPosition = gtaoViewSpacePosition(uv, viewZ, gtao);
    float3 viewVector = safeNormalizeOr(-pixelCenterPosition, float3(0.0, 0.0, -1.0));
    // Nudge the shaded point a hair toward the camera so a tap landing on the very same texel can
    // never register as an occluder of itself.
    pixelCenterPosition = gtaoViewSpacePosition(uv, viewZ * 0.99999, gtao);

    float effectRadius = gtao.radiusFalloffPowerThin.x;
    float falloffRangeFraction = gtao.radiusFalloffPowerThin.y;
    float finalValuePower = gtao.radiusFalloffPowerThin.z;
    float thinOccluderCompensation = gtao.radiusFalloffPowerThin.w;
    float sampleDistributionPower = gtao.sampleParams.x;
    float depthMipSamplingOffset = gtao.sampleParams.y;
    uint sliceCount = uint(gtao.sampleParams.z);
    uint stepsPerSlice = uint(gtao.sampleParams.w);

    // Near-field bounding: taps lose influence over the last `falloffRangeFraction` of the radius
    // and contribute nothing beyond it, so a distant wall that happens to be in the depth buffer
    // cannot darken this pixel. weight = saturate(distance * falloffMul + falloffAdd).
    float falloffRange = max(falloffRangeFraction * effectRadius, SFT_EPSILON);
    float falloffFrom = effectRadius * (1.0 - falloffRangeFraction);
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float2 noise = gtaoSpatialNoise(uint2(pixel));
    float noiseSlice = noise.x;
    float noiseSample = noise.y;

    // View-space size of one pixel at this depth, used to convert the world-space effect radius
    // into a screen-space search radius.
    float pixelViewSizeAtCenterZ = viewZ * abs(gtao.ndcToViewMulAdd.x) * viewportPixelSize.x;
    float screenspaceRadius = effectRadius / max(pixelViewSizeAtCenterZ, SFT_EPSILON);

    float visibility = 0.0;
    // Surfaces far enough away that the whole AO radius covers a handful of pixels cannot be
    // meaningfully sampled; fade toward unoccluded instead of returning undersampled noise.
    visibility += saturate((10.0 - screenspaceRadius) / 100.0) * 0.5;

    // Smallest normalized step that lands off the center texel. Without it the first tap of every
    // slice reads the shaded pixel itself and reports a horizon of ~0.
    const float pixelTooCloseThreshold = 1.3;
    float minStep = pixelTooCloseThreshold / max(screenspaceRadius, SFT_EPSILON);

    for (uint slice = 0; slice < sliceCount; ++slice) {
        float sliceK = (float(slice) + noiseSlice) / float(sliceCount);
        float phi = sliceK * SFT_PI;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        // Screen-space direction of this slice, pre-scaled to the search radius in pixels.
        float2 omega = float2(cosPhi, -sinPhi) * screenspaceRadius;

        float3 directionVector = float3(cosPhi, sinPhi, 0.0);
        float3 orthoDirectionVector = directionVector - dot(directionVector, viewVector) * viewVector;
        float3 axisVector = safeNormalizeOr(cross(orthoDirectionVector, viewVector), float3(0.0, 1.0, 0.0));

        // Normal projected into the slice plane; its length is the paper's per-slice weight and
        // `n` is the angle between it and the view vector (paper lines 11-15).
        float3 projectedNormal = viewspaceNormal - axisVector * dot(viewspaceNormal, axisVector);
        float projectedNormalLength = length(projectedNormal);
        float signNorm = sign(dot(orthoDirectionVector, projectedNormal));
        float cosNorm = saturate(dot(projectedNormal, viewVector) / max(projectedNormalLength, SFT_EPSILON));
        float n = signNorm * gtaoFastAcos(cosNorm);

        // Starting horizons sit at the hemisphere boundary of the *normal*, not at -1. -1 would
        // mean "occluded from straight behind the surface", which is both unreachable and, once a
        // tap is faded out, the thing that manufactures halos.
        float lowHorizonCos0 = cos(n + SFT_PI * 0.5);
        float lowHorizonCos1 = cos(n - SFT_PI * 0.5);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        for (uint step = 0; step < stepsPerSlice; ++step) {
            // R1 sequence offset, decorrelating step positions between slices without touching the
            // frame index.
            float stepBaseNoise = float(slice + step * stepsPerSlice) * 0.6180339887498948482;
            float stepNoise = frac(noiseSample + stepBaseNoise);

            // x^2 distribution: crevices and contact points a few pixels away carry far more of the
            // visible AO signal than anything near the radius edge, so the samples go there.
            float s = (float(step) + stepNoise) / float(stepsPerSlice);
            s = pow(s, sampleDistributionPower);
            s += minStep;

            float2 sampleOffsetPixels = s * omega;
            float sampleOffsetLength = length(sampleOffsetPixels);
            float mipLevel = clamp(log2(max(sampleOffsetLength, 1.0)) - depthMipSamplingOffset,
                                   0.0, float(SFT_GTAO_DEPTH_MIP_LEVELS - 1));
            // Snapping to whole pixels keeps the reconstructed slope honest: an unsnapped offset
            // reads a texel whose center is elsewhere, which tilts the horizon by a sub-pixel
            // amount that reads as shimmer on high-frequency geometry.
            float2 sampleOffset = round(sampleOffsetPixels) * viewportPixelSize;

            float2 sampleUv0 = uv + sampleOffset;
            float2 sampleUv1 = uv - sampleOffset;
            float sampleZ0 = viewDepthPyramid.SampleLevel(gtaoPointSampler, saturate(sampleUv0), mipLevel);
            float sampleZ1 = viewDepthPyramid.SampleLevel(gtaoPointSampler, saturate(sampleUv1), mipLevel);
            float3 samplePosition0 = gtaoViewSpacePosition(sampleUv0, sampleZ0, gtao);
            float3 samplePosition1 = gtaoViewSpacePosition(sampleUv1, sampleZ1, gtao);

            float3 sampleDelta0 = samplePosition0 - pixelCenterPosition;
            float3 sampleDelta1 = samplePosition1 - pixelCenterPosition;
            float sampleDistance0 = max(length(sampleDelta0), SFT_EPSILON);
            float sampleDistance1 = max(length(sampleDelta1), SFT_EPSILON);
            float3 sampleHorizonVector0 = sampleDelta0 / sampleDistance0;
            float3 sampleHorizonVector1 = sampleDelta1 / sampleDistance1;

            // Thin-occluder heuristic. The depth buffer is a height field and cannot say how thick
            // a surface is; stretching the falloff along view depth makes a tap that sits well
            // *behind* the shaded point drop out sooner than one beside it. Tuned conservatively -
            // over-thick assumptions produce black halos around railings and foliage, and missing
            // some occlusion reads far better than occlusion that is visibly not there.
            float falloffBase0 = length(float3(sampleDelta0.xy, sampleDelta0.z * (1.0 + thinOccluderCompensation)));
            float falloffBase1 = length(float3(sampleDelta1.xy, sampleDelta1.z * (1.0 + thinOccluderCompensation)));
            float weight0 = saturate(falloffBase0 * falloffMul + falloffAdd);
            float weight1 = saturate(falloffBase1 * falloffMul + falloffAdd);
            weight0 *= screenEdgeFade(sampleUv0, gtao);
            weight1 *= screenEdgeFade(sampleUv1, gtao);

            float sampleHorizonCos0 = dot(sampleHorizonVector0, viewVector);
            float sampleHorizonCos1 = dot(sampleHorizonVector1, viewVector);
            sampleHorizonCos0 = lerp(lowHorizonCos0, sampleHorizonCos0, weight0);
            sampleHorizonCos1 = lerp(lowHorizonCos1, sampleHorizonCos1, weight1);

            horizonCos0 = max(horizonCos0, sampleHorizonCos0);
            horizonCos1 = max(horizonCos1, sampleHorizonCos1);
        }

        // Paper equation 7: closed-form cosine-weighted arc integral for the two horizons.
        float h0 = -gtaoFastAcos(horizonCos1);
        float h1 = gtaoFastAcos(horizonCos0);
        float sinN = sin(n);
        float arc0 = (cosNorm + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cosNorm + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
        visibility += projectedNormalLength * (arc0 + arc1);
    }

    visibility /= float(sliceCount);
    visibility = pow(saturate(visibility), finalValuePower);
    // Never fully black: the pixel is by definition visible to the camera, and a hard 0 only makes
    // the denoiser's job harder at the bottom of crevices.
    visibility = max(visibility, 0.03);
    // The user-facing intensity dial is applied here, before the denoiser, on purpose: a lerp
    // toward 1 commutes with the denoiser's normalized weighted average, so the result is identical
    // either way, and doing it here means the raw buffer is directly comparable to the denoised one
    // (which is what the denoise toggle exists for).
    ambientOcclusionOut[pixel] = saturate(lerp(1.0, visibility, gtao.depthLinearizeEdgeIntensity.w));
}
