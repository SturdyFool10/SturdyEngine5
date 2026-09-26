// Compute probe for the heightfield projection blocks: traces a buffer of rays and writes the hits.
//
// Not part of the frame. It exists so the shader blocks in sturdy_heightfield.slang can be checked
// against the CPU reference (Renderer/Displacement/HeightfieldTrace.cpp) on a real device: upload the
// same heightfield, hierarchy and rays, dispatch this, and compare `hits` with what
// trace_hierarchical()/trace_cell_exact()/trace_parallax_occlusion() return for the same rays.
// SFT_HF_ALGORITHM selects the block under test exactly as it does in a material shader.

import sturdy_common;
import sturdy_heightfield;

struct ProbeRay {
    float4 originT;    // xyz origin, w unused
    float4 direction;  // xyz direction, w maxT
};

struct ProbeSettings {
    int2 texSize;
    uint maxSteps;
    uint hierarchyLevels;
    float heightScale;
    float referenceHeight;
    float2 tileSize;
    uint wrap;
    uint pomSteps;
    uint pomRefinements;
    uint rayCount;
    // Hierarchy level the hierarchical walk starts at; only read with SFT_HF_START_LEVEL_FROM_FOOTPRINT.
    uint startLevel;
};

HfTexture heightTexture;
HfTexture hierarchyTexture;
ConstantBuffer<ProbeSettings> settings;
StructuredBuffer<ProbeRay> rays;
RWStructuredBuffer<HeightfieldHit> hits;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeMain(uint3 id : SV_DispatchThreadID) {
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
    s.pomSteps = settings.pomSteps;
    s.pomRefinements = settings.pomRefinements;
    s.startLevel = settings.startLevel;

    ProbeRay r = rays[id.x];
    HeightfieldRay ray;
    ray.origin = r.originT.xyz;
    ray.direction = r.direction.xyz;
    ray.maxT = r.direction.w;

// Numeric values, not the SFT_HF_* names: preprocessor macros defined inside an imported module
// are not visible to the importer. Values match HeightfieldAlgorithm (2 = POM, 3 = cell-exact,
// 4 = hierarchical).
#ifndef SFT_HF_ALGORITHM
#define SFT_HF_ALGORITHM 4
#endif
#if SFT_HF_ALGORITHM == 2
    hits[id.x] = hf_trace_pom(heightTexture, s, ray);
#elif SFT_HF_ALGORITHM == 3
    hits[id.x] = hf_trace_grid(heightTexture, hierarchyTexture, false, s, ray);
#else
    hits[id.x] = hf_trace_grid(heightTexture, hierarchyTexture, true, s, ray);
#endif
}
