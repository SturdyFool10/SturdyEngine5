// Basic RHI material shader for GeometryVertex.
// The renderer's material pipeline feeds position/normal/uv/color at locations 0..3.
// This pass-through shader is useful for debug geometry and as the simplest material baseline.
struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

struct SceneDrawConstants {
    float4x4 view_projection;
    float4x4 model;
};

[[push_constant]] ConstantBuffer<SceneDrawConstants> scene_draw;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    float4 world_position = mul(float4(input.position, 1.0), scene_draw.model);
    output.position = mul(world_position, scene_draw.view_projection);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
    return input.color;
}
