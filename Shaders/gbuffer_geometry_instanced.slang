// Instanced vertex stage for the GPU-driven batch-draw path (Renderer::record_instanced_batches,
// RendererGpuCulling.cpp). Paired at pipeline-creation time with gbuffer_geometry.slang's existing,
// unmodified fragmentMain — see Renderer::instanced_pipeline_for's doc comment for why this lives in
// its own file/compile unit rather than as a second entry point inside gbuffer_geometry.slang: a
// second entry point in that file would add these bindings to *every* pipeline variant built from
// it (including the plain per-item vertexMain/depthOnlyMain draws, which don't use them), forcing
// every material instance's draw — batched or not — to also bind a compatible descriptor set for
// them. Compiling this vertex stage as an independent module keeps the existing per-item material
// pipeline layout completely untouched; the two are linked into one graphics pipeline whose layout
// combines the material's own set 0 (reused as-is) with a small, hand-declared set 1 for the two
// buffers below.
//
// VertexOutput's field order must exactly match gbuffer_geometry.slang's VertexOutput — SPIR-V
// vertex/fragment interface matching between separately-compiled modules is by declaration order
// (location), not by struct name.

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 tangent : TANGENT;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 worldTangent : TANGENT;
    float4 currentClip : TEXCOORD1;
    float4 previousClip : TEXCOORD2;
};

struct SceneObjectGpuData {
    float4x4 model;
    float4x4 previousModel;
    float4 idSortVisibilityFlags;
};

struct InstancedDrawConstants {
    float4x4 viewProjection;
    // Own, separate push constant from gbuffer_geometry.slang's scene_draw (this file is its own
    // compile unit — see header comment), so adding a second mat4 here doesn't touch that struct's
    // 128-byte budget. Together with viewProjection this is still exactly 128 bytes.
    float4x4 previousViewProjection;
};

[[push_constant]] ConstantBuffer<InstancedDrawConstants> instancedDraw;

// Explicit set=1, matching where Renderer::instanced_pipeline_for places
// InstanceCullResources::instance_data_bind_group_layout in the combined pipeline layout
// (RendererGpuCulling.cpp) — deliberately not reflected as part of the material template's own set
// 0, since this layout is fixed/known ahead of time, not derived from a material's own
// parameters/textures. Without this annotation, Slang's default Vulkan binding assignment puts
// every loose global-scope resource in a standalone-compiled shader at set 0, which collides with
// the material's own set-0 layout at the *other* index of that combined pipeline layout (caught at
// pipeline-creation time as VUID-VkGraphicsPipelineCreateInfo-layout-07990: descriptor type
// mismatch between the shader's compiled-in set and the bind-group layout actually provided at that
// set index — see Shaders/gbuffer_geometry_history.slang's identical fix for the non-instanced path,
// where this was caught).
[[vk::binding(0, 1)]]
StructuredBuffer<SceneObjectGpuData> sceneObjects;
[[vk::binding(1, 1)]]
StructuredBuffer<uint> compactedIndices;

[shader("vertex")]
VertexOutput vertexMainInstanced(VertexInput input, uint instanceId : SV_InstanceID) {
    uint objectIndex = compactedIndices[instanceId];
    SceneObjectGpuData object = sceneObjects[objectIndex];
    float4x4 model = object.model;

    VertexOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0), model);
    output.position = mul(worldPosition, instancedDraw.viewProjection);
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.uv = input.uv;
    output.color = input.color;
    output.worldTangent = float4(normalize(mul(input.tangent.xyz, (float3x3)model)), input.tangent.w);

    output.currentClip = output.position;
    float4 previousWorldPosition = mul(float4(input.position, 1.0), object.previousModel);
    output.previousClip = mul(previousWorldPosition, instancedDraw.previousViewProjection);
    return output;
}
