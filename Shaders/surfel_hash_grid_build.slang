// Rebuilds the surfel spatial hash grid every frame from the current (post-seed) surfel ring buffer.
// Single-pass fixed-capacity scatter: cells beyond kSurfelGridCellCapacity entries simply drop the
// overflow rather than chaining, trading a little coverage for avoiding a prefix-sum pass.
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
StructuredBuffer<SurfelGpuData> surfelBuffer;
RWStructuredBuffer<SurfelGridCell> gridBuffer;

uint hashSurfelCell(int3 cell, uint bucketCount) {
    uint h = (uint)(cell.x * 73856093) ^ (uint)(cell.y * 19349663) ^ (uint)(cell.z * 83492791);
    return h % max(bucketCount, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void hashGridBuildMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint maxSurfels = (uint)frame.cameraPositionMaxSurfels.w;
    if (dispatchThreadId.x >= maxSurfels) return;

    SurfelGpuData surfel = surfelBuffer[dispatchThreadId.x];
    if (surfel.positionRadius.w <= 0.0) return;

    float cellSize = max(frame.gridParams.y, 0.01);
    uint bucketCount = (uint)frame.gridParams.x;
    int3 cell = int3(floor(surfel.positionRadius.xyz / cellSize));
    uint cellIndex = hashSurfelCell(cell, bucketCount);

    uint slot;
    InterlockedAdd(gridBuffer[cellIndex].count, 1u, slot);
    if (slot < 8u) {
        gridBuffer[cellIndex].indices[slot] = dispatchThreadId.x;
    }
}
