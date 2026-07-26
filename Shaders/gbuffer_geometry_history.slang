// Non-instanced, per-item vertex stage for the deferred G-buffer geometry pass's per-object motion
// vectors (Renderer::history_pipeline_for, RendererObjectHistory.cpp). Paired at pipeline-creation
// time with gbuffer_geometry.slang's existing fragmentMain — same reason
// gbuffer_geometry_instanced.slang lives in its own file/compile unit (see that file's header
// comment): a second vertex entry point declared inside gbuffer_geometry.slang itself would pull
// this file's sceneObjects/sceneView bind group into *every* pipeline variant built from that
// module, including the z-prepass/shadow (depth-only) pipelines and any other material that never
// asks for object history. Compiling this as an independent module keeps gbuffer_geometry.slang's
// own vertexMain/SceneDrawConstants push constant (128 bytes — Vulkan's guaranteed minimum, no room
// for a previous_model mat4 too) completely untouched.
//
// VertexOutput's field order must exactly match gbuffer_geometry.slang's VertexOutput — SPIR-V
// vertex/fragment interface matching between separately-compiled modules is by declaration order
// (location), not by struct name. See that file for the currentClip/previousClip fields' meaning.

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

struct SceneViewGpuData {
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4 cameraWorldPositionNear;
    float4 ambientRadianceExposure;
    float4 farFovObjectCountTime;
};

struct ObjectHistoryDrawConstants {
    uint objectIndex;
};

[[push_constant]] ConstantBuffer<ObjectHistoryDrawConstants> objectDraw;

// Explicit set=1 (matching where Renderer::history_pipeline_for places this shader's hand-built
// bind-group layout in the combined pipeline layout — RendererObjectHistory.cpp) — without this,
// Slang's default Vulkan binding assignment puts every loose global-scope resource in a shader
// compiled from a standalone module at set 0, which collides with the material's own set-0 layout
// at the *other* index in that combined pipeline layout (caught at pipeline-creation time as
// VUID-VkGraphicsPipelineCreateInfo-layout-07990: descriptor type mismatch between the shader's
// compiled-in set and the bind-group layout actually provided at that set index). Deliberately not
// reflected as part of the material template's own set 0 — Renderer::ensure_object_history_resources
// creates this layout by hand, not through generate_bind_group_layouts, since it's fixed and known
// ahead of time. Backed by the exact same per-frame view_buffer/object_buffer prepare_scene_gpu_data
// already fills for the instanced path (RendererScene.cpp) — real content, not a placeholder.
[[vk::binding(0, 1)]]
StructuredBuffer<SceneObjectGpuData> sceneObjects;
// StructuredBuffer, not ConstantBuffer<SceneViewGpuData>: observed empirically that Slang's
// [[vk::binding(N, M)]] only reliably honors the requested *set* (M) for StructuredBuffer-kind
// resources — a ConstantBuffer<T> here silently ignored the set argument and landed back at set 0,
// colliding with the material's own set-0 layout (caught as "entry binding is not present in the
// bind group's layout" when this file's set-1 layout only had sceneObjects's entry). A single-element
// StructuredBuffer is functionally equivalent for a one-off per-frame read; view_buffer's creation
// (RendererScene.cpp) was widened to BufferUsage::Storage to match.
[[vk::binding(1, 1)]]
StructuredBuffer<SceneViewGpuData> sceneView;

[shader("vertex")]
VertexOutput vertexMainWithHistory(VertexInput input) {
    SceneObjectGpuData object = sceneObjects[objectDraw.objectIndex];
    SceneViewGpuData view = sceneView[0];

    VertexOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0), object.model);
    output.position = mul(worldPosition, view.viewProjection);
    output.worldNormal = normalize(mul(input.normal, (float3x3)object.model));
    output.uv = input.uv;
    output.color = input.color;
    output.worldTangent = float4(normalize(mul(input.tangent.xyz, (float3x3)object.model)), input.tangent.w);

    output.currentClip = output.position;
    float4 previousWorldPosition = mul(float4(input.position, 1.0), object.previousModel);
    output.previousClip = mul(previousWorldPosition, view.previousViewProjection);
    return output;
}
