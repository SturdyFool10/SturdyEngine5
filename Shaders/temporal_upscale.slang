// Temporal anti-aliasing and upscaling (TAAU): one compute pass that reconstructs an output-resolution image from a
// jittered, possibly lower-resolution frame plus the previous output.
//
// Current frame: the 3x3 input pixels around the output pixel's position are weighted by a Gaussian of their
// distance to it, measured where each input pixel *actually sampled the scene* (its centre minus this frame's
// sub-pixel jitter), so successive frames fill in different sub-pixel positions. Weights are luminance-tonemapped
// (Karis' 1 / (1 + luma)) so a lone HDR highlight cannot dominate.
//
// History: reprojected through the G-buffer motion of the nearest-depth input pixel of the 3x3 (dilation keeps
// silhouettes of moving objects crisp), with the jitter difference removed from the motion (the motion vectors are
// computed with the jittered matrices), sampled with a 5-tap Catmull-Rom filter, and clamped to the variance box of
// the current neighbourhood in YCoCg -- which is what rejects stale history on disocclusion or changing lighting.
//
// The new frame's weight scales with how close a real sample landed to this output pixel, so when upscaling, pixels
// between samples lean on history and converge over the jitter cycle.
import sturdy_common;

struct UpscaleConstants {
    float2 inputSize;
    float2 outputSize;
    // Screen-UV shift of the scene this frame / last frame caused by the projection jitter.
    float2 jitterUv;
    float2 previousJitterUv;
    float currentWeight;   // blend weight of the new frame for a sample exactly at the pixel centre
    float sharpness;       // 0 = bilinear history, 1 = full Catmull-Rom
    uint hasHistory;
    uint pad0;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<UpscaleConstants> upscale;
#else
[[push_constant]] ConstantBuffer<UpscaleConstants> upscale;
#endif

Texture2D<float4> sceneColor;
DepthTexture2D sceneDepth;
Texture2D<float2> gbufferMotion;
Texture2D<float4> historyColor;
SamplerState historySampler;
[format("rgba16f")]
WTexture2D<float4> upscaledOut;

float3 rgbToYCoCg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 yCoCgToRgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// 5-tap Catmull-Rom (Jimenez): four bilinear taps plus the centre, corners dropped.
float3 sampleHistory(float2 uv) {
    float2 samplePosition = uv * upscale.outputSize;
    float2 texPos1 = floor(samplePosition - 0.5) + 0.5;
    float2 f = samplePosition - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, float2(1.0e-6, 1.0e-6));
    float2 texel = 1.0 / upscale.outputSize;
    float2 texPos0 = (texPos1 - 1.0) * texel;
    float2 texPos3 = (texPos1 + 2.0) * texel;
    float2 texPos12 = (texPos1 + offset12) * texel;
    float3 result = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    result += historyColor.SampleLevel(historySampler, float2(texPos12.x, texPos0.y), 0.0).rgb * (w12.x * w0.y);
    result += historyColor.SampleLevel(historySampler, float2(texPos0.x, texPos12.y), 0.0).rgb * (w0.x * w12.y);
    result += historyColor.SampleLevel(historySampler, texPos12, 0.0).rgb * (w12.x * w12.y);
    result += historyColor.SampleLevel(historySampler, float2(texPos3.x, texPos12.y), 0.0).rgb * (w3.x * w12.y);
    result += historyColor.SampleLevel(historySampler, float2(texPos12.x, texPos3.y), 0.0).rgb * (w12.x * w3.y);
    weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    float3 catmullRom = max(result / max(weight, 1.0e-6), float3(0.0, 0.0, 0.0));
    float3 bilinear = historyColor.SampleLevel(historySampler, uv, 0.0).rgb;
    return lerp(bilinear, catmullRom, saturate(upscale.sharpness));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void upscaleMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 outputPixel = dispatchThreadId.xy;
    if (any((float2)outputPixel >= upscale.outputSize)) return;
    float2 uv = ((float2)outputPixel + 0.5) / upscale.outputSize;

    // Input pixel i sampled the scene at uv (i + 0.5) / inputSize - jitterUv.
    float2 inputPosition = (uv + upscale.jitterUv) * upscale.inputSize - 0.5;
    int2 centerInput = int2(round(inputPosition));
    int2 maxInput = int2(upscale.inputSize) - int2(1, 1);
    // Gaussian footprint grows with the upscale ratio so every output pixel sees some sample.
    float scale = max(upscale.inputSize.x / upscale.outputSize.x, 0.5);
    float sharpnessFactor = 2.29 * scale * scale;

    float3 sum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;
    float maxWeight = 0.0;
    float3 moment1 = float3(0.0, 0.0, 0.0);
    float3 moment2 = float3(0.0, 0.0, 0.0);
    float3 boxMin = float3(1.0e10, 1.0e10, 1.0e10);
    float3 boxMax = float3(-1.0e10, -1.0e10, -1.0e10);
    float closestDepth = 1.0;
    int2 closestPixel = clamp(centerInput, int2(0, 0), maxInput);
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int2 pixel = clamp(centerInput + int2(dx, dy), int2(0, 0), maxInput);
            float3 color = max(sceneColor.Load(int3(pixel, 0)).rgb, float3(0.0, 0.0, 0.0));
            float2 delta = (float2)(centerInput + int2(dx, dy)) - inputPosition;
            float weight = exp(-sharpnessFactor * dot(delta, delta)) / (1.0 + luminance(color));
            sum += color * weight;
            weightSum += weight;
            maxWeight = max(maxWeight, exp(-sharpnessFactor * dot(delta, delta)));
            float3 ycocg = rgbToYCoCg(color / (1.0 + luminance(color)));
            moment1 += ycocg;
            moment2 += ycocg * ycocg;
            boxMin = min(boxMin, ycocg);
            boxMax = max(boxMax, ycocg);
            float depth = sceneDepth.Load(int3(pixel, 0));
            if (depth < closestDepth) {
                closestDepth = depth;
                closestPixel = pixel;
            }
        }
    }
    float3 current = weightSum > 1.0e-6 ? sum / weightSum
                                        : max(sceneColor.Load(int3(clamp(centerInput, int2(0, 0), maxInput), 0)).rgb, float3(0.0, 0.0, 0.0));

    float3 result = current;
    if (upscale.hasHistory != 0u) {
        float2 motionUv = motionToUvDelta(gbufferMotion.Load(int3(closestPixel, 0))) -
                          (upscale.jitterUv - upscale.previousJitterUv);
        float2 previousUv = uv - motionUv;
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            float3 history = sampleHistory(previousUv);
            // Variance clipping in the same tonemapped YCoCg space the box was built in.
            float3 mean = moment1 / 9.0;
            float3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, float3(0.0, 0.0, 0.0)));
            float3 clipMin = max(boxMin, mean - 1.25 * sigma);
            float3 clipMax = min(boxMax, mean + 1.25 * sigma);
            float3 historyMapped = rgbToYCoCg(history / (1.0 + luminance(history)));
            historyMapped = clamp(historyMapped, clipMin, clipMax);
            float3 clampedRgb = yCoCgToRgb(historyMapped);
            history = clampedRgb / max(1.0 - luminance(clampedRgb), 1.0e-3);

            float alpha = clamp(upscale.currentWeight * maxWeight, 0.02, 1.0);
            // Blend in tonemapped space (inverse after) to keep HDR edges from ringing.
            float3 currentMapped = current / (1.0 + luminance(current));
            float3 historyMappedRgb = history / (1.0 + luminance(history));
            float3 blended = lerp(historyMappedRgb, currentMapped, alpha);
            result = blended / max(1.0 - luminance(blended), 1.0e-3);
        }
    }
    upscaledOut.Store(outputPixel, float4(max(result, float3(0.0, 0.0, 0.0)), 1.0));
}
