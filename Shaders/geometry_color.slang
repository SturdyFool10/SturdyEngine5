// Basic RHI material shader for GeometryVertex.
// The renderer's material pipeline feeds position/normal/uv/color at locations 0..3.
// This pass-through shader is useful for debug geometry and as the simplest material baseline.
import sturdy_common;

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

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<SceneDrawConstants> scene_draw;

#else

[[push_constant]] ConstantBuffer<SceneDrawConstants> scene_draw;

#endif
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
