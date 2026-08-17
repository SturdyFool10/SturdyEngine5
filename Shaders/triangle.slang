// Vertex-buffer-fed RHI shader: position/color come from the renderer's bound RHI vertex buffer.
// Locations are pinned explicitly to match the RHI vertex input layout (position at 0, color at 1)
// rather than relying on compiler default attribute ordering.
//
// Not wired to any C++ pipeline today (only referenced from doc-comment examples in
// Core/Slang/ShaderSource.hpp etc.) — a minimal standalone example, not part of the main render
// path.
import sturdy_common;

struct VertexInput {
    float2 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
