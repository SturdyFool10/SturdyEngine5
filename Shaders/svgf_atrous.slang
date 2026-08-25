// SVGF pass 2/2: one a-trous ("with holes") wavelet edge-stopping filter iteration over the color+
// variance produced by svgf_temporal_accumulate.slang (iteration 0) or the previous iteration
// (iterations 1..N-1). Dispatched `RestirGiSettings::svgf_atrous_iterations` times by
// `Renderer::build_svgf_denoiser_module`, with the step size doubling each call (1, 2, 4, 8, 16, ...)
// so the effective filter footprint grows geometrically without the O(iterations^2) cost of a
// literally-growing dense kernel. Edge-stopping weights reject taps whose normal, depth, or luminance
// diverge too far from the center pixel's — luminance divergence is judged relative to a small 3x3-
// blurred estimate of the variance channel (not the raw noisy per-pixel variance), matching the SVGF
// paper's own stabilization of that term.
//
// Iteration 0 (`stepSize == 1`) additionally writes its filtered result to the persistent color-history
// texture (`historyColorOut`, gated by the `writeHistory` push-constant flag): SVGF deliberately feeds
// the *once-filtered* image back as next frame's temporal blend target rather than the raw
// temporally-accumulated color, which is what lets it keep accumulating effectively wider spatial
// support across frames without needing to store or re-derive a growing kernel. Variance/moments are
// never spatially filtered before being stored as history — svgf_temporal_accumulate.slang already
// wrote those directly from its own (unfiltered) raw computation.
import sturdy_common;

struct SvgfFrameConstants {
    float4x4 inverseViewProjection;
    float4x4 previousViewProjection;
    float4 extentHistoryValidFrameIndex;
    float4 temporalPhiParams;
};

struct SvgfAtrousConstants {
    uint stepSize;
    uint writeHistory;
};

ConstantBuffer<SvgfFrameConstants> frame;
[[vk::push_constant]] ConstantBuffer<SvgfAtrousConstants> atrousConstants;
Texture2D<float2> gbufferNormal;
Texture2D<float> gbufferDepth;
Texture2D<float4> colorVarianceIn;
RWTexture2D<float4> colorVarianceOut;
RWTexture2D<float4> historyColorOut;

static const float3 SFT_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
static const float kKernel1D[3] = {0.375, 0.25, 0.0625}; // center, 1-tap, 2-tap weights of {1,4,6,4,1}/16

float blurredVariance(uint2 pixel, uint2 extent) {
    float sum = 0.0;
    float weightSum = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 tap = int2(pixel) + int2(dx, dy);
            if (any(tap < int2(0, 0)) || any(tap >= int2(extent))) continue;
            float w = (dx == 0 && dy == 0) ? 4.0 : ((dx == 0 || dy == 0) ? 2.0 : 1.0);
            sum += colorVarianceIn.Load(int3(tap, 0)).a * w;
            weightSum += w;
        }
    }
    return weightSum > 0.0 ? sum / weightSum : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void atrousMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 extent = (uint2)frame.extentHistoryValidFrameIndex.xy;
    if (any(dispatchThreadId.xy >= extent)) return;

    float4 center = colorVarianceIn.Load(int3(dispatchThreadId.xy, 0));
    float centerDepth = gbufferDepth.Load(int3(dispatchThreadId.xy, 0));
    if (centerDepth >= 0.999999) {
        colorVarianceOut[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 0.0);
        if (atrousConstants.writeHistory != 0u) historyColorOut[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 centerNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(dispatchThreadId.xy, 0)));
    float centerLuminance = dot(center.rgb, SFT_LUMINANCE_WEIGHTS);
    float lumSigma = sqrt(max(blurredVariance(dispatchThreadId.xy, extent), 0.0));

    float phiNormal = max(frame.temporalPhiParams.y, 1.0);
    float phiDepth = max(frame.temporalPhiParams.z, 1.0e-4);
    float phiLuminance = max(frame.temporalPhiParams.w, 1.0e-4);
    int step = max((int)atrousConstants.stepSize, 1);

    float3 totalColor = center.rgb;
    float totalVariance = center.a;
    float totalWeight = 1.0;
    float totalVarianceWeight = 1.0;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy) {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2 tap = int2(dispatchThreadId.xy) + int2(dx, dy) * step;
            if (any(tap < int2(0, 0)) || any(tap >= int2(extent))) continue;

            float tapDepth = gbufferDepth.Load(int3(tap, 0));
            if (tapDepth >= 0.999999) continue;
            float4 tapColorVariance = colorVarianceIn.Load(int3(tap, 0));
            float3 tapNormal = decodeOctahedralNormal(gbufferNormal.Load(int3(tap, 0)));
            float tapLuminance = dot(tapColorVariance.rgb, SFT_LUMINANCE_WEIGHTS);

            float kernelWeight = kKernel1D[min(abs(dx), 2)] * kKernel1D[min(abs(dy), 2)];
            float normalWeight = pow(saturate(dot(tapNormal, centerNormal)), phiNormal);
            float depthWeight = exp(-abs(centerDepth - tapDepth) / phiDepth);
            float luminanceWeight = exp(-abs(centerLuminance - tapLuminance) / (phiLuminance * lumSigma + 1.0e-4));

            float weight = kernelWeight * normalWeight * depthWeight * luminanceWeight;
            totalColor += tapColorVariance.rgb * weight;
            totalVariance += tapColorVariance.a * weight * weight;
            totalWeight += weight;
            totalVarianceWeight += weight * weight;
        }
    }

    float3 filteredColor = totalWeight > 1.0e-6 ? totalColor / totalWeight : center.rgb;
    float filteredVariance = totalVarianceWeight > 1.0e-6 ? totalVariance / totalVarianceWeight : center.a;

    colorVarianceOut[dispatchThreadId.xy] = float4(filteredColor, filteredVariance);
    if (atrousConstants.writeHistory != 0u) {
        historyColorOut[dispatchThreadId.xy] = float4(filteredColor, filteredVariance);
    }
}
