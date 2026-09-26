// Per-object motion vectors for displaced materials: the displaced counterparts of
// gbuffer_geometry_history.slang's vertexMainWithHistory. See plans/displacement-system.md section 7.
//
// Why a separate file: displaced_build_vertex() (sturdy_displaced_surface.slang) sets
// previousClip = position, i.e. camera-motion-only — a moving displaced object would leave a zero
// object-motion vector. This module adds the previous-transform half, and lives in its own compile
// unit for the same reason gbuffer_geometry_history.slang does: its sceneObjects/sceneView bind group
// (set 1) must not leak into pipelines built from gbuffer_geometry_displaced.slang that never ask for
// object history.
//
// Entry points (vertex only; pair with gbuffer_geometry_displaced.slang's fragmentMain, same variant
// defines, exactly as the plain history vertex pairs with gbuffer_geometry.slang's fragmentMain):
//
//   vertexMainWithHistory           base-surface geometry (fragment-side projection displaces per pixel).
//                                   The previous clip is the base surface's under previousModel; the
//                                   per-pixel parallax shift of the displaced point is not in the vector
//                                   (displacement is a per-pixel refinement of the same surface, and its
//                                   own motion is second order).
//   vertexMainDisplacedWithHistory  LEGACY pre-tessellated path. The vertex is pushed along its normal by
//                                   the height at its uv; the *previous* position applies previousModel to the
//                                   base vertex and pushes it along the PREVIOUS world normal by the same
//                                   displacement, so a rigidly moving displaced vertex has exactly the
//                                   motion of its transform. If the height texture itself animates,
//                                   this uses the current height for both frames (the previous frame's texel
//                                   is not available); such a material needs its own history for that term.
//
// Bind requirements (all in addition to the material's own set 0, which is shared with
// gbuffer_geometry_displaced.slang because both import sturdy_displaced_surface):
//   * set 1, binding 0: StructuredBuffer<SceneObjectGpuData> (model, previousModel, ...)
//   * set 1, binding 1: StructuredBuffer<SceneViewGpuData>   (viewProjection, previousViewProjection)
//     -- the same hand-built layout/buffers Renderer::ensure_object_history_resources already creates
//     for the plain history path; nothing new to allocate.
//   * push constant / SFT_EMULATE_PUSH_CONSTANTS uniform at set 3: ObjectHistoryDrawConstants (object index).
// D3D12 spelling of the explicit registers matches gbuffer_geometry_history.slang's.

import sturdy_common;
import sturdy_heightfield;
import sturdy_displaced_surface;

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 tangent : TANGENT;
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

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<ObjectHistoryDrawConstants> objectDraw : register(b0, space1);
#else
[[push_constant]] ConstantBuffer<ObjectHistoryDrawConstants> objectDraw : register(b0, space1);
#endif

[[vk::binding(0, 1)]]
StructuredBuffer<SceneObjectGpuData> sceneObjects : register(t0, space1);
[[vk::binding(1, 1)]]
StructuredBuffer<SceneViewGpuData> sceneView : register(t1, space1);

DisplacedVertexAttributes historyAttributesOf(VertexInput input) {
    DisplacedVertexAttributes a;
    a.position = input.position;
    a.normal = input.normal;
    a.uv = input.uv;
    a.color = input.color;
    a.tangent = input.tangent;
    return a;
}

// Previous clip position of the vertex displaced by `displacement` world units along its normal.
float4 displacedPreviousClip(float3 position, float3 normal, float displacement, SceneObjectGpuData object,
                             SceneViewGpuData view) {
    float3 previousNormal = normalize(mul(normal, (float3x3)object.previousModel));
    float4 previousWorld = mul(float4(position, 1.0), object.previousModel);
    previousWorld.xyz += previousNormal * displacement;
    return mul(previousWorld, view.previousViewProjection);
}

[shader("vertex")]
VertexOutput vertexMainWithHistory(VertexInput input) {
    SceneObjectGpuData object = sceneObjects[objectDraw.objectIndex];
    SceneViewGpuData view = sceneView[0];
    VertexOutput output = displaced_build_vertex(historyAttributesOf(input), object.model, view.viewProjection, 0.0);
    output.previousClip = displacedPreviousClip(input.position, input.normal, 0.0, object, view);
    return output;
}

[shader("vertex")]
VertexOutput vertexMainDisplacedWithHistory(VertexInput input) {
    SceneObjectGpuData object = sceneObjects[objectDraw.objectIndex];
    SceneViewGpuData view = sceneView[0];
    float h = displaced_vertex_height(input.uv);
    float displacement = displaced_height_offset(h);
    VertexOutput output =
        displaced_build_vertex(historyAttributesOf(input), object.model, view.viewProjection, displacement);
    output.previousClip = displacedPreviousClip(input.position, input.normal, displacement, object, view);
    return output;
}
