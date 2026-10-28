// The passes that finish a frame (or compute write) aimed at an off-screen render target
// (Renderer::add_offscreen_target_finalize_passes):
//
//   encodeMain      the frame drew linear light into a half-float intermediate; write it into a target
//                   whose format does not sRGB-encode by itself (RGB10A2Unorm), encoding in the shader.
//   downsampleMain  one level of the target's mip chain: a 2x2 box filter of the level above. Data a
//                   shader encoded is decoded before averaging and re-encoded after, so the mean is
//                   taken in linear light (an *Srgb format does both in hardware; a float format needs
//                   neither).
//
// Follows the custom post-process contract (sourceTexture / sourceSampler, one push-constant block),
// so it is built and bound by the same cross-backend code as every other fullscreen effect. The
// sampler that contract supplies is linear/clamp-to-edge; every tap here lands on a texel centre, so
// the filter returns exact texels and an odd-sized level clamps at its edge.

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

// Field order and types must match `FinalizeConstants` in Renderer/RendererRenderTargets.cpp. 16 bytes,
// all 4-byte scalars, so the push-constant (std430) and uniform-buffer (std140) layouts agree.
struct FinalizeConstants {
    uint decodeSource;
    uint encodeOutput;
    uint padding0;
    uint padding1;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants: the block is carried as a uniform buffer in the reserved set the
// WebGPU backend binds a per-draw slice of (see WebGpuCommandEncoder.cpp).
[[vk::binding(0, 3)]] ConstantBuffer<FinalizeConstants> finalize;

#else

[[push_constant]] ConstantBuffer<FinalizeConstants> finalize;

#endif

float4 sourceTexel(int2 texel, float2 sourceSize) {
    float4 value = sourceTexture.SampleLevel(sourceSampler, (float2(texel) + 0.5) / sourceSize, 0.0);
    return finalize.decodeSource != 0u ? float4(srgbToLinear(saturate(value.rgb)), value.a) : value;
}

float4 encodeOutput(float4 value) {
    return finalize.encodeOutput != 0u ? float4(linearToSrgb(saturate(value.rgb)), saturate(value.a)) : value;
}

float2 sourceSize() {
    uint width;
    uint height;
    sourceTexture.GetDimensions(width, height);
    return float2(max(width, 1u), max(height, 1u));
}

[shader("fragment")]
float4 encodeMain(VertexOutput input) : SV_Target {
    float4 value = sourceTexel(int2(input.position.xy), sourceSize());
    return encodeOutput(float4(max(value.rgb, 0.0), value.a));
}

[shader("fragment")]
float4 downsampleMain(VertexOutput input) : SV_Target {
    float2 size = sourceSize();
    int2 base = int2(input.position.xy) * 2;
    float4 sum = sourceTexel(base, size) + sourceTexel(base + int2(1, 0), size) + sourceTexel(base + int2(0, 1), size) +
                 sourceTexel(base + int2(1, 1), size);
    return encodeOutput(sum * 0.25);
}
