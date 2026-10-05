// Volumetric fog, pass 3 of 3: composite onto the scene colour.
//
// Looks up the integrated volume at each pixel's depth (sky pixels use the fog's maximum distance) and applies
// `colour * transmittance + in-scattered light`.
import sturdy_common;
import sturdy_lighting_data;
import volumetric_fog_common;

ConstantBuffer<ShadowLightingGpuData> lightingData;
ConstantBuffer<FogConstants> fog;
Texture2D<float4> sceneColor;
DepthTexture2D sceneDepth;
Texture2D<float4> integratedFog;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> sceneColorOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void applyMain(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= fog.render.xy)) {
        return;
    }
    int2 pixel = int2(id.xy);
    float4 color = sceneColor.Load(int3(pixel, 0));
    float deviceDepth = sceneDepth.Load(int3(pixel, 0));
    float2 uv = (float2(pixel) + 0.5) / fog.render.xy;

    float viewDepth = fog.depthParams.y;
    if (deviceDepth < 0.999999) {
        float4 h = mul(float4(uvToNdc(uv), deviceDepth, 1.0), lightingData.inverseViewProjection);
        float3 world = h.xyz / (abs(h.w) > SFT_EPSILON ? h.w : SFT_EPSILON);
        viewDepth = abs(mul(float4(world, 1.0), lightingData.view).z);
    }

    // Boundary b of the integrated volume is stored in slice b - 1; boundary 0 (the camera) is "no fog".
    float boundary = fogSliceCoord(fog, viewDepth);
    float2 gridUv = (float2(pixel) + 0.5) / (float2(fog.grid.xy) * fog.render.z);
    float4 fogValue;
    if (boundary < 1.0) {
        fogValue = lerp(float4(0.0, 0.0, 0.0, 1.0), fogSampleSlice(integratedFog, linearSampler, fog, gridUv, 0u), boundary);
    } else {
        fogValue = fogSampleVolume(integratedFog, linearSampler, fog, gridUv, boundary - 0.5);
    }
    sceneColorOut.Store(pixel, float4(color.rgb * fogValue.a + fogValue.rgb, color.a));
}
