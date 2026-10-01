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
import sturdy_space;
import sturdy_lens;
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

ConstantBuffer<RestirGiFrameConstants> frame;
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
StructuredBuffer<ReservoirGpuData> reservoirCurrent;
RWStructuredBuffer<ReservoirGpuData> reservoirSpatial;

struct ReservoirAccumulator {
    float3 radiance;
    float weightSum;
};

void updateReservoir(inout ReservoirAccumulator state, float3 candidateRadiance, float wi, inout uint rng) {
    float wsum = state.weightSum + wi;
    if (wsum > 0.0 && spectralRandom(rng) < wi / wsum) {
        state.radiance = candidateRadiance;
    }
    state.weightSum = wsum;
}

float3 reconstructWorldPosition(uint2 pixel, uint2 extent, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    return sturdySpaceCameraClipToWorld(clip, frame.inverseViewProjection, frame.lightCountParams.w);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spatialReuseMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 giExtent = (uint2)frame.extentMaxRayDistance.xy;
    if (any(dispatchThreadId.xy >= giExtent)) return;
    uint pixelIndex = dispatchThreadId.y * giExtent.x + dispatchThreadId.x;

    ReservoirGpuData center = reservoirCurrent[pixelIndex];
    if (!restirIsValid(center)) {
        reservoirSpatial[pixelIndex] = restirEmptyReservoir();
        return;
    }

    uint fullWidth, fullHeight;
    gbufferDepth.GetDimensions(fullWidth, fullHeight);
    uint2 fullExtent = uint2(fullWidth, fullHeight);
    uint gridScale = (uint)frame.extentMaxRayDistance.z;
    uint frameIndex = (uint)frame.cameraPositionFrameIndex.w;

    uint2 centerPixel = restirRepresentativePixel(dispatchThreadId.xy, gridScale, fullExtent, frameIndex);
    float centerDepth = gbufferDepth.Load(int3(centerPixel, 0));
    float3 centerPosition = reconstructWorldPosition(centerPixel, fullExtent, centerDepth);
    float3 centerNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(centerPixel, 0)));
    float sceneScale = max(length(centerPosition - frame.cameraPositionFrameIndex.xyz), 1.0);

    uint rng = spectralHash(pixelIndex ^ (frameIndex * 668265263u) ^ 0x9e3779b9u);

    float3 centerRadiance = restirRadiance(center);
    float centerM = restirSampleCount(center);
    ReservoirAccumulator combined = (ReservoirAccumulator)0;
    float centerWeight = restirTargetFunction(centerRadiance) * max(restirContributionWeight(center), 0.0)
                       * max(centerM, 1.0);
    updateReservoir(combined, centerRadiance, centerWeight, rng);
    float combinedM = centerM;

    uint sampleCount = (uint)max(frame.temporalSpatialParams.y, 0.0);
    // The radius is authored in G-buffer pixels; the reservoirs live on the coarser GI grid.
    float radiusCells = max(frame.temporalSpatialParams.z / (float)max(gridScale, 1u), 1.0);
    for (uint i = 0; i < sampleCount; ++i) {
        float angle = spectralRandom(rng) * (SFT_PI * 2.0);
        float radius = sqrt(spectralRandom(rng)) * radiusCells;
        int2 offset = int2(round(float2(cos(angle), sin(angle)) * radius));
        int2 neighborCell = int2(dispatchThreadId.xy) + offset;
        if (any(neighborCell < int2(0, 0)) || any(neighborCell >= int2(giExtent))) continue;

        // Read the (cheap, 8-byte) neighbour reservoir first: empty cells skip all G-buffer traffic.
        uint neighborIndex = (uint)neighborCell.y * giExtent.x + (uint)neighborCell.x;
        ReservoirGpuData neighbor = reservoirCurrent[neighborIndex];
        if (!restirIsValid(neighbor)) continue;

        uint2 neighborPixel = restirRepresentativePixel(uint2(neighborCell), gridScale, fullExtent, frameIndex);
        float neighborDepth = gbufferDepth.Load(int3(neighborPixel, 0));
        if (neighborDepth >= 0.999999) continue;
        float3 neighborNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(neighborPixel, 0)));
        if (dot(neighborNormal, centerNormal) < 0.9) continue;
        float3 neighborPosition = reconstructWorldPosition(neighborPixel, fullExtent, neighborDepth);
        float planeDistance = abs(dot(neighborPosition - centerPosition, centerNormal));
        if (planeDistance > 0.05 * sceneScale) continue;

        float3 neighborRadiance = restirRadiance(neighbor);
        float neighborM = max(restirSampleCount(neighbor), 1.0);
        float neighborWeight = restirTargetFunction(neighborRadiance) * max(restirContributionWeight(neighbor), 0.0)
                             * neighborM;
        updateReservoir(combined, neighborRadiance, neighborWeight, rng);
        combinedM += neighborM;
    }

    float pHatFinal = restirTargetFunction(combined.radiance);
    float contributionWeight = pHatFinal > 1.0e-6 ? combined.weightSum / (combinedM * pHatFinal) : 0.0;
    reservoirSpatial[pixelIndex] = restirMakeReservoir(combined.radiance, contributionWeight, combinedM);
}
