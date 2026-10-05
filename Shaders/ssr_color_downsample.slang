// Screen-space reflections: one level of the blurred lit-colour chain (`reflections_history` runs this for levels 1..4).
//
// The 13-tap dual-filter downsample of Jimenez ("Next Generation Post Processing in Call of Duty: Advanced Warfare", 2014):
// a tent-weighted blur that is stable under sub-pixel motion (no fireflies, no shimmer), so the glossy reflections read
// as smooth gradients rather than as boxy blocks. Each output texel samples the source around the corner shared by its
// 2x2 source texels.
import sturdy_common;

struct DownsampleConstants {
    uint2 outputExtent;
    uint2 padding;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<DownsampleConstants> downsample;
#else
[[push_constant]] ConstantBuffer<DownsampleConstants> downsample;
#endif

Texture2D<float4> source;
SamplerState linearSampler;
[format("rgba16f")]
WTexture2D<float4> destinationOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void downsampleMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= downsample.outputExtent)) {
        return;
    }
    uint width, height;
    source.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(id.xy) + 0.5) / float2(downsample.outputExtent);

    float3 a = source.SampleLevel(linearSampler, uv + texel * float2(-2.0, 2.0), 0.0).rgb;
    float3 b = source.SampleLevel(linearSampler, uv + texel * float2(0.0, 2.0), 0.0).rgb;
    float3 c = source.SampleLevel(linearSampler, uv + texel * float2(2.0, 2.0), 0.0).rgb;
    float3 d = source.SampleLevel(linearSampler, uv + texel * float2(-2.0, 0.0), 0.0).rgb;
    float3 e = source.SampleLevel(linearSampler, uv, 0.0).rgb;
    float3 f = source.SampleLevel(linearSampler, uv + texel * float2(2.0, 0.0), 0.0).rgb;
    float3 g = source.SampleLevel(linearSampler, uv + texel * float2(-2.0, -2.0), 0.0).rgb;
    float3 h = source.SampleLevel(linearSampler, uv + texel * float2(0.0, -2.0), 0.0).rgb;
    float3 i = source.SampleLevel(linearSampler, uv + texel * float2(2.0, -2.0), 0.0).rgb;
    float3 j = source.SampleLevel(linearSampler, uv + texel * float2(-1.0, 1.0), 0.0).rgb;
    float3 k = source.SampleLevel(linearSampler, uv + texel * float2(1.0, 1.0), 0.0).rgb;
    float3 l = source.SampleLevel(linearSampler, uv + texel * float2(-1.0, -1.0), 0.0).rgb;
    float3 m = source.SampleLevel(linearSampler, uv + texel * float2(1.0, -1.0), 0.0).rgb;

    float3 result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    destinationOut.Store(int2(id.xy), float4(result, 1.0));
}
