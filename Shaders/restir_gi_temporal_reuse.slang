// ReSTIR GI pass 2/4: reprojects each pixel's previous-frame reservoir via the G-buffer's per-pixel
// motion vector (rejecting it outright when the reprojected UV leaves the screen or the history
// reservoir was never written) and merges it with this frame's fresh candidate reservoir via weighted
// reservoir sampling. The merged reservoir is written to
// `reservoirCurrent`, which becomes next frame's `reservoirPrevious` (the render-graph module ping-pongs
// the two persistent buffers each frame) — this capped-history temporal accumulation is the main
// stability lever ReSTIR GI is chosen for over surfel GI's per-surfel exponential blend.
import sturdy_common;
import sturdy_spectral_common;

struct ReservoirGpuData {
    float4 samplePositionPdf;
    float4 sampleRadianceNormalX;
    float4 normalYValid;
    float4 weightM;
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
    RestirGiPackedLight lights[8];
};

ConstantBuffer<RestirGiFrameConstants> frame;
Texture2D<float2> gbufferMotion;
StructuredBuffer<ReservoirGpuData> reservoirPrevious;
// Doubles as this pass's input (initialSampleMain already wrote this frame's fresh candidate here)
// and output (the temporally-combined result, read back next frame as `reservoirPrevious`) — a single
// read-then-write of the same per-thread slot, never touched by any other thread this dispatch.
RWStructuredBuffer<ReservoirGpuData> reservoirCurrent;

static const float3 SFT_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

float targetFunction(ReservoirGpuData reservoir) {
    return max(dot(reservoir.sampleRadianceNormalX.xyz, SFT_LUMINANCE_WEIGHTS), 0.0);
}

// Weighted-reservoir-sampling update (Bitterli 2020's streaming RIS reservoir): folds a candidate
// sample with pre-integrated weight `wi` into `state`, replacing the kept sample with probability
// wi / runningWeightSum so the kept sample is drawn proportional to its weight.
void updateReservoir(inout ReservoirGpuData state, ReservoirGpuData candidateSample, float wi, inout uint rng) {
    float wsum = state.weightM.x + wi;
    if (wsum > 0.0 && spectralRandom(rng) < wi / wsum) {
        state.samplePositionPdf = candidateSample.samplePositionPdf;
        state.sampleRadianceNormalX = candidateSample.sampleRadianceNormalX;
        state.normalYValid = candidateSample.normalYValid;
    }
    state.weightM.x = wsum;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalReuseMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= extent)) return;
    uint pixelIndex = dispatchThreadId.y * extent.x + dispatchThreadId.x;

    ReservoirGpuData candidate = reservoirCurrent[pixelIndex];
    if (candidate.normalYValid.y < 0.5) {
        reservoirCurrent[pixelIndex] = (ReservoirGpuData)0;
        return;
    }

    uint rng = spectralHash(pixelIndex ^ ((uint)frame.cameraPositionFrameIndex.w * 2246822519u));

    ReservoirGpuData combined = (ReservoirGpuData)0;
    float candidateWeight = targetFunction(candidate) * max(candidate.weightM.y, 0.0) * max(candidate.weightM.z, 1.0);
    updateReservoir(combined, candidate, candidateWeight, rng);
    float combinedM = candidate.weightM.z;

    bool hasHistory = frame.lightCountParams.z > 0.5;
    if (hasHistory) {
        float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(extent);
        float2 motion = gbufferMotion.Load(int3(dispatchThreadId.xy, 0));
        float2 previousUv = uv - motion;
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            int2 previousPixel = int2(previousUv * float2(extent));
            previousPixel = clamp(previousPixel, int2(0, 0), int2(extent) - int2(1, 1));
            uint previousIndex = (uint)previousPixel.y * extent.x + (uint)previousPixel.x;
            ReservoirGpuData history = reservoirPrevious[previousIndex];
            bool geometryValid = history.normalYValid.y > 0.5;

            if (geometryValid) {
                // Bound temporal lag: cap history's contribution to `temporalHistoryMax` equivalent
                // samples so a reservoir cannot keep an increasingly stale sample forever, which is
                // what makes disocclusion ghosting resolve within a bounded number of frames rather
                // than lingering.
                float historyMax = max(frame.temporalSpatialParams.x, 1.0);
                float historyM = min(history.weightM.z, historyMax);
                float historyWeight = targetFunction(history) * max(history.weightM.y, 0.0) * max(historyM, 1.0);
                updateReservoir(combined, history, historyWeight, rng);
                combinedM += historyM;
            }
        }
    }

    combined.weightM.z = combinedM;
    float pHatFinal = targetFunction(combined);
    combined.weightM.y = pHatFinal > 1.0e-6 ? combined.weightM.x / (combinedM * pHatFinal) : 0.0;
    combined.normalYValid.y = 1.0;
    reservoirCurrent[pixelIndex] = combined;
}
