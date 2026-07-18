// Fullscreen tonemap post-process pass.
//
// Draws a single full-screen triangle (no vertex buffer — positions come from SV_VertexID) that
// samples the HDR scene-color target and maps it to the display-referred swapchain. This is the
// first concrete post-process slot of the deferred pipeline. Reuses the reflection-driven binding
// path, while a tiny push-constant block makes the high-level graph's output policy programmable.

import sturdy_common;

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

Texture2D<float4> sceneColor;
SamplerState sceneSampler;

struct TonemapConstants {
    float exposure;
    float whitePoint;
    float saturation;
    uint operation;
};

[[push_constant]] ConstantBuffer<TonemapConstants> tonemap;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    float3 hdr = max(sceneColor.Sample(sceneSampler, input.uv).rgb, 0.0);
    float3 exposed = hdr * tonemap.exposure / max(tonemap.whitePoint, SFT_EPSILON);
    float3 mapped;
    if (tonemap.operation == 0) {
        mapped = saturate(exposed);
    } else if (tonemap.operation == 1) {
        mapped = reinhardTonemap(exposed);
    } else if (tonemap.operation == 2) {
        mapped = acesFittedTonemap(exposed);
    } else {
        mapped = exposureTonemap(exposed, 1.0);
    }
    float gray = luminance(mapped);
    mapped = saturate(lerp(float3(gray), mapped, tonemap.saturation));
    return float4(mapped, 1.0);
}
