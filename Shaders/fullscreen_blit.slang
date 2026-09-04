// Portable whole-texture blit: samples `sourceTexture` across its full extent and writes it,
// resampled to whatever size the destination render target is, covering the destination's full
// extent. This is what backs Renderer::add_texture_blit_pass, the cross-backend replacement for
// RHI::CommandEncoder::blit_texture -- Vulkan is the only backend with a native scaled/filtered
// blit command; D3D12 and WebGPU have none, so this shader is what makes a resampling copy possible
// there. Follows the same sourceTexture/sourceSampler convention as every other
// Renderer::ensure_custom_post_process-driven shader (see fullscreen_anti_aliasing.slang), so it is
// consumed by the exact same already-cross-backend pipeline-creation and binding code rather than
// needing any of its own.
//
// Deliberately has no parameters: RenderGraphBlitDesc describes a whole-source-to-whole-destination
// copy with a filter choice and nothing else, so there is nothing to pass as a push constant. The
// filter is a sampler property, not a shader one -- Renderer::add_texture_blit_pass picks the
// sampler, not this shader.

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

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    // SampleLevel (not Sample): this runs in a plain full-screen pass with no derivatives to form
    // between invocations that would make an implicit-LOD Sample meaningful, and forcing level 0
    // keeps behavior identical across every backend and every source texture regardless of its own
    // mip count.
    return sourceTexture.SampleLevel(sourceSampler, input.uv, 0.0);
}
