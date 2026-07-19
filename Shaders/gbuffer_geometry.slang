// Deferred G-buffer geometry shader for GeometryVertex.
// Outputs material attributes into multiple dynamic-rendering color attachments:
//   SV_Target0: albedo + alpha
//   SV_Target1: octahedral-encoded world normal (RG16F — see sturdy_common's
//               encodeOctahedralNormal/decodeOctahedralNormal)
//   SV_Target2: material properties (roughness, metallic, ambient occlusion, flags)

import sturdy_common;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct GBufferOutput {
    float4 albedo : SV_Target0;
    float2 normal : SV_Target1;
    float4 material : SV_Target2;
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
    output.worldNormal = normalize(mul(input.normal, (float3x3)scene_draw.model));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input) {
    GBufferOutput output;
    float3 normal = normalize(input.worldNormal);
    output.albedo = input.color;
    output.normal = encodeOctahedralNormal(normal);
    // Temporarily glossy baseline until physically-based material templates expose these per asset:
    // roughness=0.12, metallic=0.08, ambient occlusion=1.0, flags=0.0.
    output.material = float4(0.12, 0.08, 1.0, 0.0);
    return output;
}
