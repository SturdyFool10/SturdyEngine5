// Compute probe for the *point-query* halves of the heightfield blocks: filtered-equivalent height and
// gradient, single-offset parallax, and the self-shadow ray. Sibling of heightfield_trace_probe.slang
// (which covers the view-ray traversals); both are driven by Renderer/tests/DisplacementGpuParityTest.cpp
// and compared with the CPU reference in Renderer/Displacement/HeightfieldTrace.cpp.
//
// One query per thread; `mode` is a runtime value so a single compiled module covers all of them:
//   0  sample     originT.xy = uv                 -> hits[i].height / .gradient at that uv
//   1  parallax   originT.xy = uv, direction.xyz = viewTS  -> hf_parallax()
//   2  shadow     originT.xy = uv, originT.z = surface height, originT.w = bias,
//                 direction.xyz = lightTS          -> hits[i].status = 1 when blocked (cell-exact traversal)
//   3  shadow     as 2, hierarchical traversal
//
// The full HeightfieldHit is written for modes 0/1; modes 2/3 only fill `status` (1 = blocked, 0 = lit).

import sturdy_common;
import sturdy_heightfield;

struct QueryIn {
    float4 originT;
    float4 direction;
};

struct QuerySettings {
    int2 texSize;
    uint maxSteps;
    uint hierarchyLevels;
    float heightScale;
    float referenceHeight;
    float2 tileSize;
    uint wrap;
    uint mode;
    uint queryCount;
    uint pad0;
};

HfTexture heightTexture;
HfTexture hierarchyTexture;
ConstantBuffer<QuerySettings> settings;
StructuredBuffer<QueryIn> queries;
RWStructuredBuffer<HeightfieldHit> hits;

[shader("compute")]
[numthreads(64, 1, 1)]
void queryMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= settings.queryCount) {
        return;
    }
    HeightfieldSettings s;
    s.texSize = settings.texSize;
    s.maxSteps = settings.maxSteps;
    s.hierarchyLevels = settings.hierarchyLevels;
    s.heightScale = settings.heightScale;
    s.referenceHeight = settings.referenceHeight;
    s.tileSize = settings.tileSize;
    s.wrap = settings.wrap;
    s.pomSteps = 0;
    s.pomRefinements = 0;

    QueryIn q = queries[id.x];
    HeightfieldHit hit;
    hit.status = 0;
    hit.t = 0.0;
    hit.uv = float2(0.0, 0.0);
    hit.height = 0.0;
    hit.gradient = float2(0.0, 0.0);
    hit.residual = 0.0;
    hit.steps = 0;

    if (settings.mode == 0) {
        hit.uv = q.originT.xy;
        hit.height = hf_sample_height(heightTexture, q.originT.xy, s);
        hit.gradient = hf_sample_gradient(heightTexture, q.originT.xy, s);
    } else if (settings.mode == 1) {
        hit = hf_parallax(heightTexture, s, q.originT.xy, q.direction.xyz);
    } else {
        bool blocked = hf_trace_shadow(heightTexture, hierarchyTexture, settings.mode == 3, s, q.originT.xy,
                                       q.originT.z, q.direction.xyz, q.originT.w);
        hit.status = blocked ? 1 : 0;
    }
    hits[id.x] = hit;
}
