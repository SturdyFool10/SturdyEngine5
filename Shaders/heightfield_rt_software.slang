// Software fallback of the ray-tracing displacement slot: intersects a buffer of rays against a list of
// displaced triangles in a plain compute shader. No acceleration structure, no ray query — it runs on any
// device that runs compute (WebGPU included). Every ray tests every triangle (each rejected by its prism
// first), so it is a reference/probe tracer and a floor for tiny scenes, not a production BVH replacement;
// the point of the rung is that displaced secondary rays never *require* hardware RT.
//
// Also the GPU side of the parity check against Renderer/Displacement/RayTracingReference.cpp: upload the
// same heightfield, hierarchy, triangles and rays, dispatch, compare `hits` with
// intersect_displaced_triangle over the same triangles (closest wins).
//
// SFT_HF_RT_USE_HIERARCHY selects the hierarchical block (needs the hierarchy texture); default off.

import sturdy_common;
import sturdy_heightfield;
import heightfield_rt_prism;

struct SoftwareCounts {
    uint rayCount;
    uint triangleCount;
    uint2 reserved;
};

HfTexture heightTexture;
HfTexture hierarchyTexture;
ConstantBuffer<HfRtDisplacementRecord> displacement;
ConstantBuffer<SoftwareCounts> counts;
StructuredBuffer<HfRtProbeRay> rays;
StructuredBuffer<HfRtPackedTriangle> triangles;
RWStructuredBuffer<HfRtProbeHit> hits;

[shader("compute")]
[numthreads(64, 1, 1)]
void softwareTraceMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= counts.rayCount) {
        return;
    }
    HfRtParams params = hf_rt_params_from_record(displacement);
    HfRtProbeRay r = rays[id.x];
    float3 o = r.originTMin.xyz;
    float3 d = r.directionTMax.xyz;
    float tMin = r.originTMin.w;
    float tMax = r.directionTMax.w;

    HfRtProbeHit best = hf_rt_empty_probe_hit();
    bool found = false;
    for (uint i = 0u; i < counts.triangleCount; ++i) {
        HfRtTriangle tri = hf_rt_unpack_triangle(triangles[i]);
#ifdef SFT_HF_RT_USE_HIERARCHY
        HfRtHit h = hf_rt_intersect_triangle(heightTexture, hierarchyTexture, true, params, tri, o, d, tMin, tMax);
#else
        HfRtHit h = hf_rt_intersect_triangle(heightTexture, hierarchyTexture, false, params, tri, o, d, tMin, tMax);
#endif
        if (h.status != HF_STATUS_MISS && (!found || h.t < best.t)) {
            best = hf_rt_pack_probe_hit(h, i);
            found = true;
        }
    }
    hits[id.x] = best;
}
