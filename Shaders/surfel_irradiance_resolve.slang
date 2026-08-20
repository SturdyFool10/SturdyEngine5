// Screen-space resolve: for every gbuffer pixel, gathers nearby surfels from the spatial hash grid
// (the pixel's cell plus its 26 neighbors) and writes a confidence- and normal-weighted average of
// their accumulated irradiance, sampled by the deferred shadow lighting pass as indirect diffuse.
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
StructuredBuffer<SurfelGpuData> surfelBuffer;
StructuredBuffer<SurfelGridCell> gridBuffer;
RWTexture2D<float4> irradianceOut;

float3 reconstructWorldPositionResolve(uint2 pixel, float depth) {
    float2 uv = (float2(pixel) + 0.5) / frame.extentFrameIndex.xy;
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    float4 world = mul(clip, frame.inverseViewProjection);
    return world.xyz / max(world.w, 1.0e-6);
}

uint hashSurfelCell(int3 cell, uint bucketCount) {
    uint h = (uint)(cell.x * 73856093) ^ (uint)(cell.y * 19349663) ^ (uint)(cell.z * 83492791);
    return h % max(bucketCount, 1u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void irradianceResolveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentFrameIndex.xy;
    if (any(dispatchThreadId.xy >= extent)) return;

    float depth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    if (depth >= 0.999999) {
        irradianceOut[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float3 worldPosition = reconstructWorldPositionResolve(dispatchThreadId.xy, depth);
    float3 normal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));
    float cellSize = max(frame.gridParams.y, 0.01);
    uint bucketCount = (uint)frame.gridParams.x;
    float searchRadiusMultiplier = max(frame.temporalIntensity.z, 0.01);

    float3 totalIrradiance = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    int3 baseCell = int3(floor(worldPosition / cellSize));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int3 cell = baseCell + int3(dx, dy, dz);
                uint cellIndex = hashSurfelCell(cell, bucketCount);
                SurfelGridCell gridCell = gridBuffer[cellIndex];
                uint count = min(gridCell.count, 8u);
                for (uint i = 0; i < count; ++i) {
                    SurfelGpuData surfel = surfelBuffer[gridCell.indices[i]];
                    if (surfel.positionRadius.w <= 0.0) continue;
                    float searchRadius = surfel.positionRadius.w * searchRadiusMultiplier;
                    float d = distance(surfel.positionRadius.xyz, worldPosition);
                    if (d > searchRadius) continue;
                    float weight = saturate(dot(surfel.normalCascade.xyz, normal)) *
                                   smoothstep(searchRadius, 0.0, d) *
                                   surfel.irradianceConfidence.w;
                    totalIrradiance += surfel.irradianceConfidence.rgb * weight;
                    totalWeight += weight;
                }
            }
        }
    }

    float3 resolved = totalWeight > 0.0001 ? totalIrradiance / totalWeight : float3(0.0, 0.0, 0.0);
    irradianceOut[dispatchThreadId.xy] = float4(resolved, 1.0);
}
