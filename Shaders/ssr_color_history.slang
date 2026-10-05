// Screen-space reflections: level 0 of the lit-colour chain, written at the end of every frame for the next frame's trace.
//
// Half resolution (box-downsampled 2x), exposure divided back out (the lighting pass applies it, and reflected radiance
// must be scene-linear), non-finite values dropped, the brightest texels clamped so one glint cannot light a room.
//
// The alpha channel is the *view depth of the nearest surface in the texel* (0 for sky). Next frame the trace compares it with
// the depth of the surface a reflection ray hit, reprojected here through the motion vector: if they disagree, the hit
// surface was not on screen at that spot last frame (it was hidden, or off-screen), so the colour stored here is not its
// colour and the reflection is culled instead of showing something else.
import sturdy_common;
import sturdy_lighting_data;
import ssr_common;

struct HistoryConstants {
    float inverseExposure;
    float maxLuminance;
    uint2 outputExtent;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<HistoryConstants> history;
#else
[[push_constant]] ConstantBuffer<HistoryConstants> history;
#endif

ConstantBuffer<ShadowLightingGpuData> lightingData;
Texture2D<float4> sceneColor;
DepthTexture2D sceneDepth;
[format("rgba16f")]
WTexture2D<float4> historyOut;

float3 fetchColor(int2 pixel, int2 maxPixel) {
    float3 c = sceneColor.Load(int3(min(pixel, maxPixel), 0)).rgb;
    return all(c == c) && all(abs(c) < 65000.0) ? max(c, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

float fetchDepth(int2 pixel, int2 maxPixel) {
    return sceneDepth.Load(int3(min(pixel, maxPixel), 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void historyMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= history.outputExtent)) return;
    uint width, height;
    sceneColor.GetDimensions(width, height);
    int2 maxPixel = int2(int(width) - 1, int(height) - 1);
    int2 base = int2(id.xy) * 2;

    float3 c = 0.25 * (fetchColor(base, maxPixel) + fetchColor(base + int2(1, 0), maxPixel) +
                       fetchColor(base + int2(0, 1), maxPixel) + fetchColor(base + int2(1, 1), maxPixel));
    c *= history.inverseExposure;
    float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (luminance > history.maxLuminance) c *= history.maxLuminance / luminance;

    float nearest = min(min(fetchDepth(base, maxPixel), fetchDepth(base + int2(1, 0), maxPixel)),
                        min(fetchDepth(base + int2(0, 1), maxPixel), fetchDepth(base + int2(1, 1), maxPixel)));
    float viewDepth = 0.0;
    if (nearest < 0.999999) {
        float2 uv = (float2(base) + 1.0) / float2(width, height);
        viewDepth = ssrViewDepth(lightingData, ssrWorldFromDepth(lightingData, uv, nearest));
    }
    historyOut.Store(int2(id.xy), float4(c, viewDepth));
}
