// Fullscreen post-effect used by the `hello` example to exercise `sturdy::render::GraphStage`:
// multiplies the scene-linear HDR color by a tint supplied through the push-constant block.
// Same shape as the engine's own Shaders/runtime_bloom_threshold.slang.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

Texture2D<float4> sourceTexture;
SamplerState sourceSampler;

struct TintConstants {
    float4 tint;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<TintConstants> settings;
#else
[[push_constant]] ConstantBuffer<TintConstants> settings;
#endif

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float4 color = sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
    return float4(color.rgb * settings.tint.rgb, color.a);
}
