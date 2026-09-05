// Builds a Hi-Z (hierarchical max-depth) pyramid via iterative 2x2 max-reduction fullscreen passes.
//
// Mip 0 is built directly from the resolved scene depth buffer, reduced by hiz_build_from_depth.slang's
// otherwise-identical shader (kept in a separate file because the depth buffer's view (Depth32Float,
// sample type Depth) needs a DepthTexture2D binding that WebGPU cannot substitute for this file's
// plain Texture2D<float> `source` -- and Slang's parameter reflection enumerates every module-scope
// global regardless of which entry point actually references it, so the two textures can't safely
// share a module even when only one entry point per file is ever linked). Mips 1..N then each reduce
// the previous Hi-Z mip through this file. Max reduction (not min/average) is required by this
// engine's depth convention (depth_compare = Less, near = 0 / far = 1): storing the farthest
// already-covered depth per texel is what makes Renderer::record_instance_cull's occlusion test
// conservative (never wrongly culls something still visible) — see gpu_instance_cull.slang's
// occlusion test.

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct HiZBuildConstants {
    uint2 sourceExtent;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS

// WebGPU has no push constants at all, so on that target the same block is carried as an

// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of

// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant

// block is packed std430-style, a uniform buffer std140, so a struct whose members would

// pad differently between the two must be laid out to agree under both.

[[vk::binding(0, 3)]] ConstantBuffer<HiZBuildConstants> hiz;

#else

[[push_constant]] ConstantBuffer<HiZBuildConstants> hiz;

#endif
[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = fullscreenTrianglePosition(output.uv);
    return output;
}

// Single-channel float source: a previous Hi-Z pyramid mip (building mip N>0), plain Texture2D<float>,
// sampled with point `.Load` (never filtered: averaging four depths would break the max-reduction
// invariant).
Texture2D<float> source;

[shader("fragment")]
float4 reduceMain(VertexOutput input) : SV_Target {
    int2 destPixel = int2(input.position.xy);
    int2 srcBase = destPixel * 2;
    int2 maxSrc = int2(hiz.sourceExtent) - int2(1, 1);
    int2 c0 = min(srcBase, maxSrc);
    int2 c1 = min(srcBase + int2(1, 0), maxSrc);
    int2 c2 = min(srcBase + int2(0, 1), maxSrc);
    int2 c3 = min(srcBase + int2(1, 1), maxSrc);
    float d = max(max(source.Load(int3(c0, 0)), source.Load(int3(c1, 0))),
                  max(source.Load(int3(c2, 0)), source.Load(int3(c3, 0))));
    return float4(d, 0.0, 0.0, 0.0);
}
