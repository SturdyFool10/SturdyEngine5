// Hardware path of the ray-tracing displacement slot, as a compute probe: a ray query against a TLAS whose
// BLAS holds one AABB (procedural) primitive per base triangle — the triangle's prism bounds. Primitive
// index i is triangle i. Traversal hands back CANDIDATE_PROCEDURAL_PRIMITIVE for every prism box the ray
// touches; this shader runs the displaced-triangle test there and commits the hit distance, so the
// hardware BVH does the culling and the shader does only the exact surface.
//
// This is the loop a scene integration copies into traceSpectralClosestSurface & friends
// (sturdy_spectral_scene.slang): the instance for a displaced mesh points at the AABB BLAS, its custom
// index is the scene instance as usual, and CandidatePrimitiveIndex() is the triangle within the
// instance's index range. Committed procedural hits carry no barycentrics, so the caller keeps the
// HfRtHit from the candidate that committed (as `best` does below) instead of reloading the surface.
//
// Requires RayQuery + AccelerationStructures. hits[] uses the same record as the software probe, so the
// two are compared against each other and against the CPU reference.

import sturdy_common;
import sturdy_heightfield;
import heightfield_rt_prism;

struct QueryCounts {
    uint rayCount;
    uint3 reserved;
};

RaytracingAccelerationStructure sceneAccelerationStructure;
HfTexture heightTexture;
HfTexture hierarchyTexture;
ConstantBuffer<HfRtDisplacementRecord> displacement;
ConstantBuffer<QueryCounts> counts;
StructuredBuffer<HfRtProbeRay> rays;
StructuredBuffer<HfRtPackedTriangle> triangles;
RWStructuredBuffer<HfRtProbeHit> hits;

[shader("compute")]
[numthreads(64, 1, 1)]
void queryTraceMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= counts.rayCount) {
        return;
    }
    HfRtParams params = hf_rt_params_from_record(displacement);
    HfRtProbeRay r = rays[id.x];

    RayDesc ray;
    ray.Origin = r.originTMin.xyz;
    ray.Direction = r.directionTMax.xyz;
    ray.TMin = r.originTMin.w;
    ray.TMax = r.directionTMax.w;

    HfRtProbeHit best = hf_rt_empty_probe_hit();
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneAccelerationStructure, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed()) {
        if (query.CandidateType() != CANDIDATE_PROCEDURAL_PRIMITIVE) {
            continue;
        }
        uint triangleIndex = query.CandidatePrimitiveIndex();
        HfRtTriangle tri = hf_rt_unpack_triangle(triangles[triangleIndex]);
        // TMin/TMax of the query are already applied by the traversal; the candidate's own hit must also
        // beat the closest committed hit so far, which the query tracks through its current TMax.
        HfRtHit h = hf_rt_intersect_triangle(heightTexture, hierarchyTexture, true, params, tri,
                                             ray.Origin, ray.Direction, ray.TMin, query.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT ? query.CommittedRayT() : ray.TMax);
        if (h.status != HF_STATUS_MISS) {
            query.CommitProceduralPrimitiveHit(h.t);
            best = hf_rt_pack_probe_hit(h, triangleIndex);
        }
    }
    hits[id.x] = best;
}
