// Fullscreen tonemap post-process pass.
//
// Draws a single full-screen triangle (no vertex buffer — positions come from SV_VertexID) that
// samples the HDR scene-color target and maps it to the display-referred swapchain. This is the
// first concrete post-process slot of the deferred pipeline (see plans/deferred-pipeline.md); the
// G-buffer + lighting passes slot in *before* it later. Reuses the reflection-driven binding path,
// so the renderer generates its bind-group layout from these two bindings automatically.

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    // Classic attribute-less fullscreen triangle: uv sweeps (0,0),(2,0),(0,2) so the primitive covers
    // the whole [-1,1] clip rect with the screen inside its first quadrant. Vulkan NDC Y and texture V
    // both point down, so map uv straight to clip space (uv*2-1) — negating Y here would flip the image.
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::binding(1, 0)]] SamplerState sceneSampler;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 hdr = sceneColor.Sample(sceneSampler, input.uv).rgb;
    // Reinhard tonemap: HDR (unbounded) -> [0,1) display range. Cheap, stable, and enough to prove the
    // HDR-target + fullscreen-post path; ACES/filmic curves drop in here later.
    float3 mapped = hdr / (hdr + 1.0);
    return float4(mapped, 1.0);
}
