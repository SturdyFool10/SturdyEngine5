// Deferred G-buffer material with displacement — the drop-in counterpart of gbuffer_geometry.slang.
// See plans/displacement-system.md. Same G-buffer targets, same glTF material parameter/texture names
// (so a glTF-imported material still maps field-for-field), plus a height texture and, for the top
// rung, a min/max hierarchy.
//
// One source, several configurations, all chosen at compile time (a ShaderVariant define set per
// DisplacementPlan — the planner never edits this file):
//
//   SFT_HF_ALGORITHM          0 NormalOnly | 1 Parallax | 2 POM | 3 CellExact | 4 Hierarchical
//                             (default 4). The per-pixel block; see sturdy_heightfield.slang.
//   entry point vertexMainDisplaced (instead of vertexMain): the LEGACY geometry path, where a
//                             CPU-subdivided mesh (Displacement::subdivide_uniform) is displaced per
//                             vertex. Pair with SFT_HF_ALGORITHM=0 — the geometry already carries the
//                             displacement, and projecting again would apply it twice (the planner
//                             enforces the same rule).
//   SFT_DISPLACEMENT_WRITE_DEPTH  define to write the displaced depth (SV_Depth). Costs early-Z, so the
//                             renderer must keep such a material out of the depth prepass — that pass's
//                             depthOnlyMain below only knows the base surface.
//
// The mesh-shader geometry path (Shaders/displacement_mesh.slang) feeds this same fragmentMain, with
// SFT_HF_ALGORITHM=0.
//
// Unlike gbuffer_geometry.slang, the fragment stage needs the eye position. The scene view buffer that
// carries it lives in a hand-built bind group (see gbuffer_geometry_history.slang for what that costs),
// so this template takes it as an ordinary material uniform instead: the application sets
// displacement_camera_position once per frame per material.

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

struct SceneDrawConstants {
    float4x4 view_projection;
    float4x4 model;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<SceneDrawConstants> scene_draw;
#else
[[push_constant]] ConstantBuffer<SceneDrawConstants> scene_draw;
#endif

// ------------------------------------------------------------------------------------------------
// Vertex stages
// ------------------------------------------------------------------------------------------------

DisplacedVertexAttributes attributesOf(VertexInput input) {
    DisplacedVertexAttributes a;
    a.position = input.position;
    a.normal = input.normal;
    a.uv = input.uv;
    a.color = input.color;
    a.tangent = input.tangent;
    return a;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    return displaced_build_vertex(attributesOf(input), scene_draw.model, scene_draw.view_projection, 0.0);
}

// LEGACY geometry path: the input mesh is already subdivided (Displacement::subdivide_uniform), and
// each vertex is pushed along its normal by the height at its uv. SampleLevel with an explicit LOD is
// the vertex-stage-safe fetch (no derivatives exist here). Vertices duplicated across triangle edges
// share a uv and therefore a height, so the displaced surface stays crack-free.
[shader("vertex")]
VertexOutput vertexMainDisplaced(VertexInput input) {
    float h = displaced_vertex_height(input.uv);
    return displaced_build_vertex(attributesOf(input), scene_draw.model, scene_draw.view_projection,
                                  displaced_height_offset(h));
}

// ------------------------------------------------------------------------------------------------
// Fragment stage: the shared displaced-surface shading (sturdy_displaced_surface.slang)
// ------------------------------------------------------------------------------------------------

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) {
    return displaced_shade(input, frontFacing);
}

// Z prepass. Only knows the base surface: a material built with SFT_DISPLACEMENT_WRITE_DEPTH must not
// be drawn here (its G-buffer pass would then fail the "equal depth" test), and the renderer's
// depth-prepass eligibility check is where that policy belongs.
[shader("fragment")]
void depthOnlyMain(VertexOutput input) {
    float alpha = input.color.a * base_color_factor.a * base_color_texture.Sample(input.uv).a;
    if (!displaced_alpha_passes(input, alpha)) {
        discard;
    }
}
