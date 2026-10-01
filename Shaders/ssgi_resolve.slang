// Screen-space indirect lighting, pass 2 of 3: upsample + temporal accumulation.
//
// For every full-resolution pixel, gathers the 3x3 neighbourhood of half-resolution trace cells with a depth-
// aware Gaussian (cells whose depth disagrees with this pixel are dropped, so light does not bleed across
// silhouettes), then blends the result into last frame's accumulation reprojected through the motion vector. The
// history is only reused when it was accumulated on the same surface (its stored depth matches), which is what
// keeps disocclusions and moving objects from smearing.
//
// Output (rgba16f, full resolution): rgb = irradiance / pi, a = linear view depth (for next frame's validation).
import sturdy_common;

struct ResolveConstants {
    float depthLinearizeA;
    float depthLinearizeB;
    float temporalAlpha;
    uint hasHistory;
    uint2 traceExtent;
    uint2 fullExtent;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<ResolveConstants> resolve;
#else
[[push_constant]] ConstantBuffer<ResolveConstants> resolve;
#endif

DepthTexture2D sceneDepth;
Texture2D<float2> gbufferMotion;
Texture2D<float4> rawIrradiance;
Texture2D<float4> accumulatedPrevious;
[format("rgba16f")]
WTexture2D<float4> accumulatedOut;

static const float kDepthTolerance = 0.05;   // relative
static const float kHistoryDepthTolerance = 0.08;

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(dispatchThreadId.xy >= resolve.fullExtent)) return;

    float deviceDepth = sceneDepth.Load(int3(pixel, 0));
    if (deviceDepth >= 0.999999) {
        accumulatedOut.Store(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    float viewZ = resolve.depthLinearizeA / max(resolve.depthLinearizeB - deviceDepth, 1.0e-6);

    // Spatial: depth-aware 3x3 over the half-resolution cells around this pixel.
    int2 centerCell = pixel / 2;
    int2 maxCell = int2(resolve.traceExtent) - int2(1, 1);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float3 nearest = float3(0.0, 0.0, 0.0);
    float nearestDepthError = 1.0e30;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 cell = clamp(centerCell + int2(dx, dy), int2(0, 0), maxCell);
            float4 cellSample = rawIrradiance.Load(int3(cell, 0));
            if (cellSample.a <= 0.0) continue;
            float depthError = abs(cellSample.a - viewZ) / viewZ;
            if (depthError < nearestDepthError) {
                nearestDepthError = depthError;
                nearest = cellSample.rgb;
            }
            float2 cellCenter = (float2(cell) + 0.5) * 2.0;
            float distanceSquared = dot(cellCenter - (float2(pixel) + 0.5), cellCenter - (float2(pixel) + 0.5));
            float weight = exp(-distanceSquared / 8.0) * exp(-depthError / kDepthTolerance);
            sum += cellSample.rgb * weight;
            weightSum += weight;
        }
    }
    float3 current = weightSum > 1.0e-4 ? sum / weightSum : nearest;

    // Temporal.
    float3 result = current;
    if (resolve.hasHistory != 0u) {
        float2 uv = (float2(pixel) + 0.5) / float2(resolve.fullExtent);
        float2 previousUv = uv - motionToUvDelta(gbufferMotion.Load(int3(pixel, 0)));
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            int2 previousPixel = clamp(int2(previousUv * float2(resolve.fullExtent)), int2(0, 0),
                                       int2(resolve.fullExtent) - int2(1, 1));
            float4 history = accumulatedPrevious.Load(int3(previousPixel, 0));
            bool sameSurface = history.a > 0.0 && abs(history.a - viewZ) <= kHistoryDepthTolerance * viewZ + 0.01;
            if (sameSurface) {
                result = lerp(history.rgb, current, saturate(resolve.temporalAlpha));
            }
        }
    }
    accumulatedOut.Store(pixel, float4(max(result, float3(0.0, 0.0, 0.0)), viewZ));
}
