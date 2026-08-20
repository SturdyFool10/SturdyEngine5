// Spawns new surfels into G-buffer coverage gaps, amortized across frames via a tile stride so the
// worst-case number of spawn candidates considered per frame stays bounded by SurfelGiSettings::
// max_surfels_spawned_per_frame regardless of screen resolution.
import sturdy_common;

struct SurfelGpuData {
    float4 positionRadius;
    float4 normalCascade;
    float4 irradianceConfidence;
};

struct SurfelGridCell {
    uint count;
    uint indices[8];
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
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
RWStructuredBuffer<SurfelGpuData> surfelBuffer;
StructuredBuffer<SurfelGridCell> gridBuffer;
RWStructuredBuffer<uint> allocCursor;

float3 reconstructWorldPositionSeed(uint2 pixel, float depth) {
    float2 uv = (float2(pixel) + 0.5) / frame.extentFrameIndex.xy;
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

float surfelRadiusForDistance(float cameraDistance) {
    float t = saturate(cameraDistance / max(frame.cascadeDistances.w, 1.0));
    return lerp(frame.gridParams.z, frame.gridParams.w, t);
}

uint hashSurfelCell(int3 cell, uint bucketCount) {
    uint h = (uint)(cell.x * 73856093) ^ (uint)(cell.y * 19349663) ^ (uint)(cell.z * 83492791);
    return h % max(bucketCount, 1u);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void screenSeedMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentFrameIndex.xy;
    uint tilesX = (extent.x + 15u) / 16u;
    uint tilesY = (extent.y + 15u) / 16u;
    if (dispatchThreadId.x >= tilesX || dispatchThreadId.y >= tilesY) return;

    uint totalTiles = tilesX * tilesY;
    uint maxSpawnPerFrame = max(1u, (uint)frame.spawnParams.x);
    uint stride = max(1u, totalTiles / maxSpawnPerFrame);
    uint tileIndex = dispatchThreadId.y * tilesX + dispatchThreadId.x;
    uint frameIndex = (uint)frame.extentFrameIndex.z;
    if ((tileIndex + frameIndex) % stride != 0u) return;

    uint2 pixel = min(dispatchThreadId.xy * 16u + uint2(8u, 8u), extent - 1u);
    float depth = gbufferDepth.Load(int3(pixel, 0));
    if (depth >= 0.999999) return;

    float3 worldPosition = reconstructWorldPositionSeed(pixel, depth);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Load(int3(pixel, 0)));
    float cameraDistance = distance(worldPosition, frame.cameraPositionMaxSurfels.xyz);
    float radius = surfelRadiusForDistance(cameraDistance);

    uint bucketCount = (uint)frame.gridParams.x;
    float cellSize = max(frame.gridParams.y, 0.01);
    int3 cell = int3(floor(worldPosition / cellSize));
    uint cellIndex = hashSurfelCell(cell, bucketCount);

    SurfelGridCell existing = gridBuffer[cellIndex];
    uint coveredCount = 0u;
    uint checkCount = min(existing.count, 8u);
    for (uint i = 0; i < checkCount; ++i) {
        SurfelGpuData candidate = surfelBuffer[existing.indices[i]];
        if (candidate.positionRadius.w <= 0.0) continue;
        float d = distance(candidate.positionRadius.xyz, worldPosition);
        if (d < candidate.positionRadius.w * 0.75 && dot(candidate.normalCascade.xyz, normal) > 0.5) {
            coveredCount++;
        }
    }
    float coverage = checkCount > 0u ? saturate((float)coveredCount) : 0.0;
    if (coverage >= frame.spawnParams.y) return;

    uint maxSurfels = (uint)frame.cameraPositionMaxSurfels.w;
    uint claimed;
    InterlockedAdd(allocCursor[0], 1u, claimed);
    uint slot = claimed % max(maxSurfels, 1u);

    surfelBuffer[slot].positionRadius = float4(worldPosition, radius);
    surfelBuffer[slot].normalCascade = float4(normal, 0.0);
    surfelBuffer[slot].irradianceConfidence = float4(0.0, 0.0, 0.0, 0.0);
}
