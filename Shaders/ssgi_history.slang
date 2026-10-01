// Screen-space indirect lighting, pass 3 of 3: keeps a half-resolution copy of this frame's lit scene colour for
// next frame's trace to read radiance from. Box-downsampled 2x, exposure divided back out (the lighting pass
// applies it, and the radiance feeding the bounce must be scene-linear), non-finite values dropped.
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

Texture2D<float4> sceneColor;
[format("rgba16f")]
WTexture2D<float4> radianceHistoryOut;

float3 fetch(int2 pixel, int2 maxPixel) {
    float3 c = sceneColor.Load(int3(min(pixel, maxPixel), 0)).rgb;
    return all(c == c) && all(abs(c) < 65000.0) ? max(c, float3(0.0, 0.0, 0.0)) : float3(0.0, 0.0, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void historyMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (any(dispatchThreadId.xy >= history.outputExtent)) return;
    uint width, height;
    sceneColor.GetDimensions(width, height);
    int2 maxPixel = int2(int(width) - 1, int(height) - 1);
    int2 base = int2(dispatchThreadId.xy) * 2;
    float3 c = 0.25 * (fetch(base, maxPixel) + fetch(base + int2(1, 0), maxPixel) + fetch(base + int2(0, 1), maxPixel) +
                       fetch(base + int2(1, 1), maxPixel));
    c *= history.inverseExposure;
    // Clamp the brightest texels (the sun's specular glint) so a single pixel cannot light up a room.
    float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (luminance > history.maxLuminance) c *= history.maxLuminance / luminance;
    radianceHistoryOut.Store(dispatchThreadId.xy, float4(c, 1.0));
}
