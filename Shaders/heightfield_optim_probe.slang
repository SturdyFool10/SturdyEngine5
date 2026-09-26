// Compute probe for the OPTIONAL traversal optimizations in sturdy_heightfield.slang — the ones behind
// SFT_HF_USE_GATHER and SFT_HF_START_LEVEL_FROM_FOOTPRINT. Sibling of heightfield_trace_probe.slang (which
// checks the baseline blocks); this one adds the per-dispatch startLevel input the optimizations need, so a
// device run can show that neither define changes a hit (Renderer/tests/DisplacementHierarchyBuildGpuTest.cpp).
// Hits are written as HeightfieldHit, in ray order.

import sturdy_common;
import sturdy_heightfield;

struct OptimRay {
    float4 originT;   // xyz origin
    float4 direction; // xyz direction, w maxT
};

struct OptimSettings {
    int2 texSize;
    uint maxSteps;
    uint hierarchyLevels;
    float heightScale;
    float referenceHeight;
    float2 tileSize;
    uint wrap;
    uint useHierarchy;
    uint rayCount;
    uint startLevel;
};

HfTexture heightTexture;
HfTexture hierarchyTexture;
ConstantBuffer<OptimSettings> settings;
StructuredBuffer<OptimRay> rays;
RWStructuredBuffer<HeightfieldHit> hits;

[shader("compute")]
[numthreads(64, 1, 1)]
void optimProbeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= settings.rayCount) {
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
    s.startLevel = settings.startLevel;

    OptimRay r = rays[id.x];
    HeightfieldRay ray;
    ray.origin = r.originT.xyz;
    ray.direction = r.direction.xyz;
    ray.maxT = r.direction.w;
    hits[id.x] = hf_trace_grid(heightTexture, hierarchyTexture, settings.useHierarchy != 0, s, ray);
}
