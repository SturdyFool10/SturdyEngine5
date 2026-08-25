// ReSTIR GI pass 3/4: gathers a handful of nearby pixels' temporally-reused reservoirs and combines
// them into this pixel's, approximating many more traced samples than were actually shot this frame.
// Neighbors are rejected on normal/depth-plane disagreement so reuse doesn't smear light across
// unrelated surfaces (a wall vs. the floor behind it, etc). The result is written to a separate
// `reservoirSpatial` buffer rather than fed back into the temporal ping-pong buffers — feeding
// spatially-diffused reservoirs back into next frame's temporal history would let neighboring pixels'
// noise correlate and amplify over time instead of resolving, so spatial reuse is deliberately a
// dead-end each frame, read only by restir_gi_shade_resolve.slang.
//
// Like the temporal pass, this uses the simplified `pHat(y) = luminance(y.radiance)` target function
// and does not apply a solid-angle Jacobian correction when reweighting a neighbor's sample for this
// pixel's viewpoint — an accepted approximation for this pass's scope that trades a small amount of
// bias (slight energy mismatch when a reused sample point subtends a very different solid angle from
// the neighbor's viewpoint vs. this pixel's) for not having to re-derive each candidate's original
// sampling geometry.
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
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
StructuredBuffer<ReservoirGpuData> reservoirCurrent;
RWStructuredBuffer<ReservoirGpuData> reservoirSpatial;

static const float3 SFT_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

float targetFunction(ReservoirGpuData reservoir) {
    return max(dot(reservoir.sampleRadianceNormalX.xyz, SFT_LUMINANCE_WEIGHTS), 0.0);
}

void updateReservoir(inout ReservoirGpuData state, ReservoirGpuData candidateSample, float wi, inout uint rng) {
    float wsum = state.weightM.x + wi;
    if (wsum > 0.0 && spectralRandom(rng) < wi / wsum) {
        state.samplePositionPdf = candidateSample.samplePositionPdf;
        state.sampleRadianceNormalX = candidateSample.sampleRadianceNormalX;
        state.normalYValid = candidateSample.normalYValid;
    }
    state.weightM.x = wsum;
}

float3 reconstructWorldPosition(uint2 pixel, uint2 extent, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spatialReuseMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= extent)) return;
    uint pixelIndex = dispatchThreadId.y * extent.x + dispatchThreadId.x;

    ReservoirGpuData center = reservoirCurrent[pixelIndex];
    if (center.normalYValid.y < 0.5) {
        reservoirSpatial[pixelIndex] = (ReservoirGpuData)0;
        return;
    }

    float centerDepth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    float3 centerPosition = reconstructWorldPosition(dispatchThreadId.xy, extent, centerDepth);
    float3 centerNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));

    uint rng = spectralHash(pixelIndex ^ ((uint)frame.cameraPositionFrameIndex.w * 668265263u) ^ 0x9e3779b9u);

    ReservoirGpuData combined = (ReservoirGpuData)0;
    float centerWeight = targetFunction(center) * max(center.weightM.y, 0.0) * max(center.weightM.z, 1.0);
    updateReservoir(combined, center, centerWeight, rng);
    float combinedM = center.weightM.z;

    uint sampleCount = (uint)max(frame.temporalSpatialParams.y, 0.0);
    float radiusPx = max(frame.temporalSpatialParams.z, 1.0);
    for (uint i = 0; i < sampleCount; ++i) {
        float angle = spectralRandom(rng) * (SFT_PI * 2.0);
        float radius = sqrt(spectralRandom(rng)) * radiusPx;
        int2 offset = int2(round(float2(cos(angle), sin(angle)) * radius));
        int2 neighborPixel = int2(dispatchThreadId.xy) + offset;
        if (any(neighborPixel < int2(0, 0)) || any(neighborPixel >= int2(extent))) continue;

        float neighborDepth = gbufferDepth.Load(int3(neighborPixel, 0));
        if (neighborDepth >= 0.999999) continue;
        float3 neighborNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(neighborPixel, 0)));
        if (dot(neighborNormal, centerNormal) < 0.9) continue;
        float3 neighborPosition = reconstructWorldPosition(uint2(neighborPixel), extent, neighborDepth);
        float planeDistance = abs(dot(neighborPosition - centerPosition, centerNormal));
        float sceneScale = max(length(centerPosition - frame.cameraPositionFrameIndex.xyz), 1.0);
        if (planeDistance > 0.05 * sceneScale) continue;

        uint neighborIndex = (uint)neighborPixel.y * extent.x + (uint)neighborPixel.x;
        ReservoirGpuData neighbor = reservoirCurrent[neighborIndex];
        if (neighbor.normalYValid.y < 0.5) continue;

        float neighborM = max(neighbor.weightM.z, 1.0);
        float neighborWeight = targetFunction(neighbor) * max(neighbor.weightM.y, 0.0) * neighborM;
        updateReservoir(combined, neighbor, neighborWeight, rng);
        combinedM += neighborM;
    }

    combined.weightM.z = combinedM;
    float pHatFinal = targetFunction(combined);
    combined.weightM.y = pHatFinal > 1.0e-6 ? combined.weightM.x / (combinedM * pHatFinal) : 0.0;
    combined.normalYValid.y = 1.0;
    reservoirSpatial[pixelIndex] = combined;
}
