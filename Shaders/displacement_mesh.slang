// Mesh-shader geometry path for displacement: task + mesh stages that replace the legacy
// "pre-tessellated mesh + displacing vertex shader" path (gbuffer_geometry_displaced.slang's
// vertexMainDisplaced). See plans/displacement-system.md.
//
// Same fragment stage, same material, same output as the legacy path — only the way triangles reach the
// rasterizer differs, which is what "interchangeable geometry blocks" means here:
//
//   legacy      CPU subdivide_uniform() -> big vertex/index buffers -> vertex shader displaces each vertex
//   mesh        base mesh only (16 floats/vertex) -> TASK picks a level per triangle + culls
//               -> MESH emits the displaced sub-triangles straight to the rasterizer
//
// so memory is the base mesh, refinement follows the camera per frame with no CPU work, and triangles
// outside the frustum never reach vertex work. The planner only selects this path when
// RHI::Feature::MeshShader and TaskShader are present and the device's output limits admit
// MESH_MAX_LEVEL (Displacement::max_mesh_shader_subdivision); everything else takes the legacy path,
// which needs nothing beyond vertex texture fetch.
//
// Crack-free refinement without neighbour information: every triangle edge has its own level, computed
// from that edge's two endpoints alone (edgeLevel, symmetric in its arguments), so the two triangles
// sharing an edge always agree on it. A triangle's interior level is the max of its three edges; along
// each edge, vertices are snapped to that edge's own level, in canonical (lower vertex id first)
// direction and with canonical operand order, so both sides produce bit-identical points, uvs and
// heights along the shared edge. "Canonical" = from the lexicographically smaller model-space endpoint, so
// the rule survives duplicated seam vertices with different indices. Mirrors DisplacementMesh.cpp
// (edge_runs_forward / snap_edge_parameter / snapped_vertex_position), which the test suite checks for
// exactly this property.
//
// Limits this example deliberately keeps simple (see the plan for the follow-ups):
//   * one base triangle per mesh workgroup, capped at MESH_MAX_LEVEL — a triangle needing more than
//     level 8 is emitted at 8; the base mesh should be authored so that is rare;
//   * per-triangle sphere culling; meshlet-granularity cone + sphere culling would amortize better;
//   * no back-face culling in the task stage: displacement can turn a back-facing base triangle into a
//     silhouette, so only a padded frustum test is conservative.
//
// Mesh output goes through the same VertexOutput as the legacy path and feeds the same displaced_shade;
// build this with SFT_HF_ALGORITHM=0 — the geometry already carries the displacement, and projecting
// per pixel on top would apply it twice (the planner enforces the same rule).

import sturdy_common;
import sturdy_heightfield;
import sturdy_displaced_surface;

// Must equal Displacement::kTaskTrianglesPerGroup / kMeshMaxSubdivisionLevel (DisplacementMesh.hpp).
static const uint TASK_TRIANGLES_PER_GROUP = 32;
static const uint MESH_MAX_LEVEL = 8;
static const uint MESH_MAX_VERTICES = 45;   // (8+1)(8+2)/2
static const uint MESH_MAX_PRIMITIVES = 64; // 8*8

// Per view (one uniform buffer slot per distinct view of a frame: the main camera, each shadow view).
struct DisplacementViewConstants {
    float4x4 viewProjection;
    // Camera the refinement is chosen for (always the main camera, even while drawing a shadow view, so the
    // shadow silhouettes refine like the visible surface).
    float4 cameraPosition;
    // World-space frustum planes of THIS view, normals pointing inward: dot(plane.xyz, p) + plane.w >= 0 is inside.
    float4 frustumPlanes[6];
    // viewport_height / (2 * tan(fov / 2)), i.e. pixels per world unit at unit distance.
    float pixelsPerUnitAtOne;
};

// Per draw (push constants, Task|Mesh stages). Padded to 96 bytes; mirrored by DisplacementMeshGpuDrawConstants.
struct DisplacementDrawConstants {
    float4x4 model;
    // The base mesh lives in the renderer's shared geometry arenas: its first vertex (in vertices) and
    // first index (in indices) within them. Indices are relative to vertexBase, as for draw_indexed.
    uint firstIndex;
    uint vertexBase;
    uint triangleCount;
    // Largest axis scale of `model`; pads culling bounds by the displacement amplitude in world units.
    float boundsScale;
    float targetEdgePixels;
    uint maxLevel;
    uint pad0;
    uint pad1;
};

// Explicit set 1: material resources (imported from sturdy_displaced_surface) keep set 0 exactly as the
// legacy vertex path lays it out, so a material's bind groups work unchanged with either geometry path.
// register(..., space1) for D3D12, same split as gbuffer_geometry_history.slang.
[[vk::binding(0, 1)]]
ConstantBuffer<DisplacementViewConstants> mesh_view : register(b0, space1);
[[push_constant]] ConstantBuffer<DisplacementDrawConstants> mesh_draw;
// Base mesh in the engine's GeometryVertex layout, read as raw floats: a struct with float3 members
// would pick up std430's 16-byte vec3 alignment and stop matching the tightly packed CPU layout.
// 16 floats per vertex: position(3) normal(3) uv(2) color(4) tangent(4).
[[vk::binding(1, 1)]]
StructuredBuffer<float> baseVertexData : register(t1, space1);
[[vk::binding(2, 1)]]
StructuredBuffer<uint> baseIndices : register(t2, space1);

struct BaseVertex {
    float3 position;
    float3 normal;
    float2 uv;
    float4 color;
    float4 tangent;
};

uint baseIndexAt(uint triangle, uint corner) { return baseIndices[mesh_draw.firstIndex + triangle * 3 + corner]; }

BaseVertex loadBaseVertex(uint index) {
    uint b = (mesh_draw.vertexBase + index) * 16;
    BaseVertex v;
    v.position = float3(baseVertexData[b + 0], baseVertexData[b + 1], baseVertexData[b + 2]);
    v.normal = float3(baseVertexData[b + 3], baseVertexData[b + 4], baseVertexData[b + 5]);
    v.uv = float2(baseVertexData[b + 6], baseVertexData[b + 7]);
    v.color = float4(baseVertexData[b + 8], baseVertexData[b + 9], baseVertexData[b + 10], baseVertexData[b + 11]);
    v.tangent = float4(baseVertexData[b + 12], baseVertexData[b + 13], baseVertexData[b + 14], baseVertexData[b + 15]);
    return v;
}

// ------------------------------------------------------------------------------------------------
// Level selection (mirrors select_edge_level in DisplacementMesh.cpp; symmetric in a, b)
// ------------------------------------------------------------------------------------------------

uint edgeLevel(float3 a, float3 b) {
    float len = length(b - a);
    float3 mid = (a + b) * 0.5;
    float dist = max(length(mid - mesh_view.cameraPosition.xyz) - 0.5 * len, 1.0e-3);
    float edgePixels = (len + displacement_height_scale) * mesh_view.pixelsPerUnitAtOne / dist;
    float wanted = ceil(edgePixels / max(mesh_draw.targetEdgePixels, 1.0e-3));
    uint ceilingLevel = clamp(mesh_draw.maxLevel, 1u, MESH_MAX_LEVEL);
    if (!(wanted >= 1.0)) {
        return 1u;
    }
    return uint(min(wanted, float(ceilingLevel)));
}

float3 worldPositionOf(BaseVertex v) { return mul(float4(v.position, 1.0), mesh_draw.model).xyz; }

// ------------------------------------------------------------------------------------------------
// Task stage: classify 32 base triangles, keep the visible ones, record their levels
// ------------------------------------------------------------------------------------------------

struct DisplacementPayload {
    uint count;
    uint triangle[TASK_TRIANGLES_PER_GROUP];
    uint level[TASK_TRIANGLES_PER_GROUP];
};

groupshared DisplacementPayload gPayload;

[shader("amplification")]
[numthreads(TASK_TRIANGLES_PER_GROUP, 1, 1)]
void taskMain(uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
    if (lane == 0) {
        gPayload.count = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint tri = group.x * TASK_TRIANGLES_PER_GROUP + lane;
    if (tri < mesh_draw.triangleCount) {
        float3 w0 = worldPositionOf(loadBaseVertex(baseIndexAt(tri, 0)));
        float3 w1 = worldPositionOf(loadBaseVertex(baseIndexAt(tri, 1)));
        float3 w2 = worldPositionOf(loadBaseVertex(baseIndexAt(tri, 2)));

        // Bounding sphere of the base triangle, padded by the displacement amplitude in every direction.
        float3 center = (w0 + w1 + w2) * (1.0 / 3.0);
        float radius = max(max(length(w0 - center), length(w1 - center)), length(w2 - center)) +
                       displacement_height_scale * mesh_draw.boundsScale;
        bool visible = true;
        for (uint p = 0; p < 6; ++p) {
            if (dot(mesh_view.frustumPlanes[p].xyz, center) + mesh_view.frustumPlanes[p].w < -radius) {
                visible = false;
            }
        }

        if (visible) {
            uint level = max(max(edgeLevel(w0, w1), edgeLevel(w1, w2)), edgeLevel(w2, w0));
            uint slot;
            InterlockedAdd(gPayload.count, 1u, slot);
            gPayload.triangle[slot] = tri;
            gPayload.level[slot] = level;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(gPayload.count, 1, 1, gPayload);
}

// ------------------------------------------------------------------------------------------------
// Mesh stage: one workgroup emits one base triangle's displaced sub-triangles
// ------------------------------------------------------------------------------------------------

// Which way an edge is canonically walked: from its lexicographically smaller MODEL-space endpoint to the
// larger (x, then y, then z). Position, not vertex index, so duplicated vertices (uv/normal seams, welded
// copies) that share a position agree on the direction; edge_runs_forward in DisplacementMesh.cpp.
bool posLess(float3 a, float3 b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// Snap grid step `step` (of `steps`, running from endpoint a to endpoint b, `aIsOrigin` = a is the canonical
// origin) to an edge's own level and return the parameter measured from the canonical origin
// (snap_edge_parameter). Integer arithmetic, so the two triangles sharing the edge — which see it in
// opposite directions — round every tie identically; a float 1 - s mirror is not bit-exact and cracked the
// surface in testing.
float snapEdgeParameter(uint step, uint steps, uint edgeLevel, bool aIsOrigin) {
    uint n = max(steps, 1u);
    uint levels = max(edgeLevel, 1u);
    uint canonicalStep = aIsOrigin ? step : n - step;
    uint g = (2u * canonicalStep * levels + n) / (2u * n);
    return float(g) / float(levels);
}

// lerp() that returns its endpoints EXACTLY at t == 0 and t == 1. a + t * (b - a) is not bit-exact at t = 1,
// and an edge's end vertex is also reached as the start (t = 0) of the next edge of the fan around a base
// vertex, so an inexact end would leave a one-ulp gap between neighbouring triangles at every corner.
float3 exactLerp(float3 a, float3 b, float t) { return t <= 0.0 ? a : (t >= 1.0 ? b : lerp(a, b, t)); }
float2 exactLerp(float2 a, float2 b, float t) { return t <= 0.0 ? a : (t >= 1.0 ? b : lerp(a, b, t)); }
float4 exactLerp(float4 a, float4 b, float t) { return t <= 0.0 ? a : (t >= 1.0 ? b : lerp(a, b, t)); }

// Blends the three base vertices at the snapped position of grid vertex (row, col). `lodTexelsAlong`
// is the number of height texels one grid step covers here (edge vertices derive it from their edge alone).
BaseVertex blendBase(BaseVertex v0, BaseVertex v1, BaseVertex v2, uint3 edgeLevels, uint level, uint row,
                     uint col, out float lodTexelsAlong) {
    float n = float(max(level, 1u));
    uint tw;
    uint th;
    height_texture.GetDimensions(tw, th);
    float2 texSize = float2(tw, th);

    BaseVertex lo = v0;
    BaseVertex hi = v1;
    float t = 0.0;
    uint edgeLevelValue = 1u;
    bool onEdge = true;
    if (row == 0) {
        bool fwd = posLess(v0.position, v1.position);
        lo = fwd ? v0 : v1;
        hi = fwd ? v1 : v0;
        t = snapEdgeParameter(col, level, edgeLevels.x, fwd);
        edgeLevelValue = edgeLevels.x;
    } else if (col + row == level) {
        bool fwd = posLess(v1.position, v2.position);
        lo = fwd ? v1 : v2;
        hi = fwd ? v2 : v1;
        t = snapEdgeParameter(row, level, edgeLevels.y, fwd);
        edgeLevelValue = edgeLevels.y;
    } else if (col == 0) {
        bool fwd = posLess(v0.position, v2.position);
        lo = fwd ? v0 : v2;
        hi = fwd ? v2 : v0;
        t = snapEdgeParameter(row, level, edgeLevels.z, fwd);
        edgeLevelValue = edgeLevels.z;
    } else {
        onEdge = false;
    }

    BaseVertex r;
    if (onEdge) {
        r.position = exactLerp(lo.position, hi.position, t);
        r.normal = exactLerp(lo.normal, hi.normal, t);
        r.uv = exactLerp(lo.uv, hi.uv, t);
        r.color = exactLerp(lo.color, hi.color, t);
        r.tangent = float4(exactLerp(lo.tangent.xyz, hi.tangent.xyz, t), lo.tangent.w);
        float2 duv = abs(hi.uv - lo.uv) * texSize;
        lodTexelsAlong = max(duv.x, duv.y) / float(max(edgeLevelValue, 1u));
    } else {
        float wb = float(col) / n;
        float wc = float(row) / n;
        float wa = 1.0 - wb - wc;
        r.position = v0.position * wa + v1.position * wb + v2.position * wc;
        r.normal = v0.normal * wa + v1.normal * wb + v2.normal * wc;
        r.uv = v0.uv * wa + v1.uv * wb + v2.uv * wc;
        r.color = v0.color * wa + v1.color * wb + v2.color * wc;
        r.tangent = float4(v0.tangent.xyz * wa + v1.tangent.xyz * wb + v2.tangent.xyz * wc, v0.tangent.w);
        float2 e0 = abs(v1.uv - v0.uv) * texSize;
        float2 e1 = abs(v2.uv - v1.uv) * texSize;
        float2 e2 = abs(v0.uv - v2.uv) * texSize;
        float longest = max(max(max(e0.x, e0.y), max(e1.x, e1.y)), max(e2.x, e2.y));
        lodTexelsAlong = longest / n;
    }
    return r;
}

// Row of grid vertex `index` (rows have level+1-r vertices) and its column.
void gridVertexRowCol(uint index, uint level, out uint row, out uint col) {
    uint start = 0;
    row = 0;
    for (uint r = 0; r <= MESH_MAX_LEVEL; ++r) {
        uint rowSize = level + 1 - r;
        if (r > level || index < start + rowSize) {
            break;
        }
        start += rowSize;
        row = r + 1;
    }
    col = index - start;
}

[shader("mesh")]
[numthreads(MESH_MAX_PRIMITIVES, 1, 1)]
[outputtopology("triangle")]
void meshMain(uint tid : SV_GroupIndex, uint3 group : SV_GroupID, in payload DisplacementPayload payload,
              OutputVertices<VertexOutput, MESH_MAX_VERTICES> verts,
              OutputIndices<uint3, MESH_MAX_PRIMITIVES> tris) {
    uint tri = payload.triangle[group.x];
    uint level = clamp(payload.level[group.x], 1u, MESH_MAX_LEVEL);
    uint vertexCount = (level + 1) * (level + 2) / 2;
    uint primitiveCount = level * level;
    SetMeshOutputCounts(vertexCount, primitiveCount);

    BaseVertex v0 = loadBaseVertex(baseIndexAt(tri, 0));
    BaseVertex v1 = loadBaseVertex(baseIndexAt(tri, 1));
    BaseVertex v2 = loadBaseVertex(baseIndexAt(tri, 2));
    float3 w0 = worldPositionOf(v0);
    float3 w1 = worldPositionOf(v1);
    float3 w2 = worldPositionOf(v2);
    uint3 edgeLevels = uint3(edgeLevel(w0, w1), edgeLevel(w1, w2), edgeLevel(w2, w0));

    if (tid < vertexCount) {
        uint row;
        uint col;
        gridVertexRowCol(tid, level, row, col);
        float texelsAlong;
        BaseVertex b = blendBase(v0, v1, v2, edgeLevels, level, row, col, texelsAlong);

        // Explicit LOD chosen from how many height texels one grid step covers, so a sparse grid reads a
        // smoothed height instead of aliasing on single texels. Edge vertices derive it from the edge only.
        float lod = max(log2(max(texelsAlong, 1.0)), 0.0);
#ifdef SFT_EMULATE_PUSH_CONSTANTS
        // WGSL: no sampler on the height texture (HfTexture is a plain Texture2D); mesh stages are Vulkan-only, so
        // this only keeps the module compilable there. LOD 0 via Loads.
        float h = displaced_vertex_height(b.uv);
#else
        float h = height_texture.SampleLevel(b.uv, lod).r;
#endif

        DisplacedVertexAttributes a;
        a.position = b.position;
        a.normal = b.normal;
        a.uv = b.uv;
        a.color = b.color;
        a.tangent = b.tangent;
        verts[tid] = displaced_build_vertex(a, mesh_draw.model, mesh_view.viewProjection, displaced_height_offset(h));
    }

    if (tid < primitiveCount) {
        // Triangles of row r: (level - r) "up" triangles interleaved with (level - r - 1) "down" ones.
        uint remaining = tid;
        uint row = 0;
        for (uint r = 0; r < MESH_MAX_LEVEL; ++r) {
            uint inRow = 2 * (level - r) - 1;
            if (r >= level || remaining < inRow) {
                break;
            }
            remaining -= inRow;
            row = r + 1;
        }
        uint col = remaining / 2;
        bool down = (remaining & 1) == 1;

        // Vertex index of (row, col): rows above contribute level+1-k vertices each.
        uint p = row * (level + 1) - (row * (row - 1)) / 2 + col;
        uint q = (row + 1) * (level + 1) - ((row + 1) * row) / 2 + col;
        tris[tid] = down ? uint3(p + 1, q + 1, q) : uint3(p, p + 1, q);
    }
}

// ------------------------------------------------------------------------------------------------
// Fragment stage — identical to the legacy path's, so one compile unit carries the whole mesh pipeline
// ------------------------------------------------------------------------------------------------

[shader("fragment")]
GBufferOutput fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) {
    return displaced_shade(input, frontFacing);
}

[shader("fragment")]
void depthOnlyMain(VertexOutput input) {
    float alpha = input.color.a * base_color_factor.a * base_color_texture.Sample(input.uv).a;
    if (!displaced_alpha_passes(input, alpha)) {
        discard;
    }
}
