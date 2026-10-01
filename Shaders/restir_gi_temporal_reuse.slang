// ReSTIR GI pass 2/4: reprojects each pixel's previous-frame reservoir via the G-buffer's per-pixel
// motion vector (rejecting it outright when the reprojected UV leaves the screen or the history
// reservoir was never written) and merges it with this frame's fresh candidate reservoir via weighted
// reservoir sampling. The merged reservoir is written to
// `reservoirCurrent`, which becomes next frame's `reservoirPrevious` (the render-graph module ping-pongs
// the two persistent buffers each frame) — this capped-history temporal accumulation is the main
// stability lever ReSTIR GI is chosen for over surfel GI's per-surfel exponential blend.
import sturdy_common;
import sturdy_spectral_common;
import restir_gi_common;

struct RestirGiPackedLight {
    float4 positionRange;
    float4 radianceSourceRadius;
};

struct RestirGiFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 cameraPositionFrameIndex;
    // xy = GI grid extent, z = GI grid downscale relative to the G-buffer, w = max ray distance.
    float4 extentMaxRayDistance;
    float4 sunDirectionAngularRadius;
    float4 sunRadiance;
    float4 temporalSpatialParams;
    float4 lightCountParams;
    RestirGiPackedLight lights[8];
};

struct GuideGpuData {
    float4 directionStrength;
    float4 normalDistance;
};

ConstantBuffer<RestirGiFrameConstants> frame;
Texture2D<float2> gbufferMotion;
// Per-cell surface signature (normal + camera distance) written by the initial-sample pass: `Current` is
// this frame's, `Previous` last frame's. History is only reused when both cells show the same surface.
StructuredBuffer<GuideGpuData> guideGeometryPrevious;
StructuredBuffer<GuideGpuData> guideGeometryCurrent;
StructuredBuffer<ReservoirGpuData> reservoirPrevious;
// Doubles as this pass's input (initialSampleMain already wrote this frame's fresh candidate here)
// and output (the temporally-combined result, read back next frame as `reservoirPrevious`) — a single
// read-then-write of the same per-thread slot, never touched by any other thread this dispatch.
RWStructuredBuffer<ReservoirGpuData> reservoirCurrent;

// Weighted-reservoir-sampling state kept in registers for the duration of one pass.
struct ReservoirAccumulator {
    float3 radiance;
    float weightSum;
};

// Weighted-reservoir-sampling update (Bitterli 2020's streaming RIS reservoir): folds a candidate
// sample with pre-integrated weight `wi` into `state`, replacing the kept sample with probability
// wi / runningWeightSum so the kept sample is drawn proportional to its weight.
void updateReservoir(inout ReservoirAccumulator state, float3 candidateRadiance, float wi, inout uint rng) {
    float wsum = state.weightSum + wi;
    if (wsum > 0.0 && spectralRandom(rng) < wi / wsum) {
        state.radiance = candidateRadiance;
    }
    state.weightSum = wsum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalReuseMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 giExtent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= giExtent)) return;
    uint pixelIndex = dispatchThreadId.y * giExtent.x + dispatchThreadId.x;

    ReservoirGpuData candidate = reservoirCurrent[pixelIndex];
    if (!restirIsValid(candidate)) {
        reservoirCurrent[pixelIndex] = restirEmptyReservoir();
        return;
    }

    uint rng = spectralHash(pixelIndex ^ ((uint)frame.cameraPositionFrameIndex.w * 2246822519u));

    float3 candidateRadiance = restirRadiance(candidate);
    float candidateM = restirSampleCount(candidate);
    ReservoirAccumulator combined = (ReservoirAccumulator)0;
    float candidateWeight = restirTargetFunction(candidateRadiance) * max(restirContributionWeight(candidate), 0.0)
                          * max(candidateM, 1.0);
    updateReservoir(combined, candidateRadiance, candidateWeight, rng);
    float combinedM = candidateM;

    bool hasHistory = frame.lightCountParams.z > 0.5;
    if (hasHistory) {
        uint fullWidth, fullHeight;
        gbufferMotion.GetDimensions(fullWidth, fullHeight);
        uint2 fullExtent = uint2(fullWidth, fullHeight);
        uint2 pixel = restirRepresentativePixel(
            dispatchThreadId.xy, (uint)frame.extentMaxRayDistance.z, fullExtent, (uint)frame.cameraPositionFrameIndex.w);
        float2 uv = (float2(pixel) + 0.5) / float2(fullExtent);
        float2 motion = gbufferMotion.Load(int3(pixel, 0));
        float2 previousUv = uv - motionToUvDelta(motion);
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            int2 previousPixel = int2(previousUv * float2(giExtent));
            previousPixel = clamp(previousPixel, int2(0, 0), int2(giExtent) - int2(1, 1));
            uint previousIndex = (uint)previousPixel.y * giExtent.x + (uint)previousPixel.x;
            ReservoirGpuData history = reservoirPrevious[previousIndex];

            // Reject history that belongs to a different surface (disocclusion, silhouette, moving
            // object): reusing it is what smears light across the screen behind moving geometry.
            float4 currentGeometry = guideGeometryCurrent[pixelIndex].normalDistance;
            float4 previousGeometry = guideGeometryPrevious[previousIndex].normalDistance;
            bool sameSurface = previousGeometry.w > 0.0 && currentGeometry.w > 0.0 &&
                               abs(previousGeometry.w - currentGeometry.w) <= 0.06 * currentGeometry.w + 0.02 &&
                               dot(previousGeometry.xyz, currentGeometry.xyz) >= 0.88;

            if (restirIsValid(history) && sameSurface) {
                // Bound temporal lag: cap history's contribution to `temporalHistoryMax` equivalent
                // samples so a reservoir cannot keep an increasingly stale sample forever, which is
                // what makes disocclusion ghosting resolve within a bounded number of frames rather
                // than lingering.
                float3 historyRadiance = restirRadiance(history);
                float historyMax = max(frame.temporalSpatialParams.x, 1.0);
                float historyM = min(restirSampleCount(history), historyMax);
                float historyWeight = restirTargetFunction(historyRadiance)
                                    * max(restirContributionWeight(history), 0.0) * max(historyM, 1.0);
                updateReservoir(combined, historyRadiance, historyWeight, rng);
                combinedM += historyM;
            }
        }
    }

    float pHatFinal = restirTargetFunction(combined.radiance);
    float contributionWeight = pHatFinal > 1.0e-6 ? combined.weightSum / (combinedM * pHatFinal) : 0.0;
    reservoirCurrent[pixelIndex] = restirMakeReservoir(combined.radiance, contributionWeight, combinedM);
}
