// Vertex-buffer-fed shader: position/color come from a bound vertex buffer (see
// Core/Vulkan/VulkanVertex.cppm) rather than being hardcoded here. Locations are pinned explicitly
// to match VulkanVertex.cppm's VkVertexInputAttributeDescription array exactly (position at 0,
// color at 1) rather than relying on the compiler's default attribute-ordering behavior.
struct VertexInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
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
