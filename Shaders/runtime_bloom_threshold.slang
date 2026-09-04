// Runtime-only bloom threshold diagnostic. This is supplied through Renderer’s generic custom HDR
// post-process contract; no bloom debug mode is built into Engine or Renderer.

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

struct ThresholdConstants {
    float threshold;
    float softKnee;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<ThresholdConstants> settings;

#else

[[push_constant]] ConstantBuffer<ThresholdConstants> settings;

#endif
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 color = max(sourceTexture.Sample(sourceSampler, input.uv).rgb, 0.0);
    float brightness = max(color.r, max(color.g, color.b));
    float knee = max(settings.threshold * settings.softKnee, 1.0e-5);
    float soft = clamp(brightness - settings.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-5);
    float contribution = max(brightness - settings.threshold, soft) / max(brightness, 1.0e-5);
    return float4(color * contribution, 1.0);
}
