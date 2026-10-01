// ReSTIR GI pass 4/4: resolves the GI grid's final (spatially-reused) reservoirs into the same-shaped
// RGBA16Float irradiance texture surfel GI used to produce — the deferred shadow lighting pass's
// consumption side (`Shaders/deferred_shadow_lighting.slang`'s `surfelIrradiance` binding) is
// unchanged; only the producer differs.
//
// Runs at full G-buffer resolution. When the GI grid is coarser than the G-buffer (extentMaxRayDistance.z
// > 1) each output pixel is reconstructed from the four nearest grid cells with a depth/normal-aware
// bilateral filter, so a 2x downscale traces a quarter of the rays without smearing light across edges.
//
// The output is demodulated: a cosine-hemisphere-sampled reservoir estimates "irradiance / pi" as simply
// radiance * W (the cosine pdf cancels the Lambert cosine and 1/pi). Albedo and (1 - metallic) are NOT
// applied here — deferred_shadow_lighting.slang multiplies the sampled `surfelIrradiance` by them, and
// doing it in both places squared the albedo.
import sturdy_common;
import sturdy_space;
import sturdy_lens;
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
StructuredBuffer<ReservoirGpuData> reservoirSpatial;
// WGSL's texture_storage_2d requires a format in the type itself; must match the RHI::Format the
// raw-irradiance target is actually created with (RHI::Format::RGBA16Float). Write-only.
[format("rgba16f")]
WTexture2D<float4> irradianceOut;

float3 reconstructWorldPosition(uint2 pixel, uint2 extent, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(extent);
    float4 clip = float4(uvToNdc(uv), depth, 1.0);
    return sturdySpaceCameraClipToWorld(clip, frame.inverseViewProjection, frame.lightCountParams.w);
}

float3 resolvedRadiance(ReservoirGpuData reservoir) {
    return restirRadiance(reservoir) * max(restirContributionWeight(reservoir), 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void shadeResolveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint fullWidth, fullHeight;
    gbufferDepth.GetDimensions(fullWidth, fullHeight);
    uint2 fullExtent = uint2(fullWidth, fullHeight);
    if (any(dispatchThreadId.xy >= fullExtent)) return;

    float depth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    if (depth >= 0.999999) {
        irradianceOut.Store(dispatchThreadId.xy, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    int2 giExtent = int2((uint2)frame.extentMaxRayDistance.xy);
    uint gridScale = max((uint)frame.extentMaxRayDistance.z, 1u);
    uint frameIndex = (uint)frame.cameraPositionFrameIndex.w;
    float gain = max(frame.temporalSpatialParams.w, 0.0);

    if (gridScale == 1u) {
        ReservoirGpuData reservoir = reservoirSpatial[dispatchThreadId.y * (uint)giExtent.x + dispatchThreadId.x];
        float3 indirect = restirIsValid(reservoir) ? resolvedRadiance(reservoir) * gain : float3(0.0, 0.0, 0.0);
        irradianceOut.Store(dispatchThreadId.xy, float4(max(indirect, float3(0.0, 0.0, 0.0)), 1.0));
        return;
    }

    float3 centerNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));
    float3 centerPosition = reconstructWorldPosition(dispatchThreadId.xy, fullExtent, depth);
    float sceneScale = max(length(centerPosition - frame.cameraPositionFrameIndex.xyz), 1.0);

    // Grid-cell coordinates of this pixel, where cell c is centred at (c + 0.5) * gridScale pixels.
    float2 cellPosition = (float2(dispatchThreadId.xy) + 0.5) / (float)gridScale - 0.5;
    int2 baseCell = (int2)floor(cellPosition);
    float2 blend = cellPosition - float2(baseCell);

    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float3 nearestFallback = float3(0.0, 0.0, 0.0);
    float nearestBlend = -1.0;
    [unroll]
    for (int tap = 0; tap < 4; ++tap) {
        int2 tapOffset = int2(tap & 1, tap >> 1);
        int2 cell = clamp(baseCell + tapOffset, int2(0, 0), giExtent - int2(1, 1));
        float bilinear = (tapOffset.x == 0 ? 1.0 - blend.x : blend.x) * (tapOffset.y == 0 ? 1.0 - blend.y : blend.y);

        ReservoirGpuData reservoir = reservoirSpatial[(uint)cell.y * (uint)giExtent.x + (uint)cell.x];
        if (!restirIsValid(reservoir)) continue;
        float3 tapRadiance = resolvedRadiance(reservoir);
        if (bilinear > nearestBlend) {
            nearestBlend = bilinear;
            nearestFallback = tapRadiance;
        }

        uint2 tapPixel = restirRepresentativePixel(uint2(cell), gridScale, fullExtent, frameIndex);
        float tapDepth = gbufferDepth.Load(int3(tapPixel, 0));
        if (tapDepth >= 0.999999) continue;
        float3 tapNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(tapPixel, 0)));
        float normalWeight = pow(saturate(dot(tapNormal, centerNormal)), 16.0);
        float3 tapPosition = reconstructWorldPosition(tapPixel, fullExtent, tapDepth);
        float planeDistance = abs(dot(tapPosition - centerPosition, centerNormal));
        float depthWeight = exp(-planeDistance / (0.01 * sceneScale));
        float weight = bilinear * normalWeight * depthWeight;
        sum += tapRadiance * weight;
        weightSum += weight;
    }

    // Every tap rejected (thin geometry, silhouettes): fall back to the closest valid cell rather than
    // leaving a hole the denoiser would have to invent.
    float3 indirect = weightSum > 1.0e-4 ? sum / weightSum : (nearestBlend >= 0.0 ? nearestFallback : float3(0.0, 0.0, 0.0));
    indirect *= gain;
    irradianceOut.Store(dispatchThreadId.xy, float4(max(indirect, float3(0.0, 0.0, 0.0)), 1.0));
}
