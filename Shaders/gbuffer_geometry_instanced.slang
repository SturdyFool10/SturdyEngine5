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
};

struct SceneObjectGpuData {
    float4x4 model;
    float4x4 previousModel;
    float4 idSortVisibilityFlags;
};

struct InstancedDrawConstants {
    float4x4 viewProjection;
};

[[push_constant]] ConstantBuffer<InstancedDrawConstants> instancedDraw;

// Set 1 (see this file's header comment) — deliberately not reflected as part of the material
// template's own set 0. Renderer::ensure_instance_cull_resources creates this layout by hand
// (RendererGpuCulling.cpp) rather than through generate_bind_group_layouts, since it's fixed and
// known ahead of time, not derived from a material's own parameters/textures.
StructuredBuffer<SceneObjectGpuData> sceneObjects;
StructuredBuffer<uint> compactedIndices;

[shader("vertex")]
VertexOutput vertexMainInstanced(VertexInput input, uint instanceId : SV_InstanceID) {
    uint objectIndex = compactedIndices[instanceId];
    float4x4 model = sceneObjects[objectIndex].model;

    VertexOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0), model);
    output.position = mul(worldPosition, instancedDraw.viewProjection);
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.uv = input.uv;
    output.color = input.color;
    output.worldTangent = float4(normalize(mul(input.tangent.xyz, (float3x3)model)), input.tangent.w);
    return output;
}
