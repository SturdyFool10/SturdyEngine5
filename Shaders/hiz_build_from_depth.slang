// Builds mip 0 of the Hi-Z pyramid directly from the real resolved scene depth buffer. Split out
// from hiz_build.slang (which handles mips 1..N, reducing a previous Hi-Z mip) because the two
// sources have incompatible WebGPU sample types: the real depth buffer's view is Depth32Float
// (sample type Depth, needs a DepthTexture2D binding), while a Hi-Z pyramid mip is a plain
// R32Float color texture (sample type Float, needs Texture2D<float>). Vulkan/D3D12 don't care and
// would happily let one Texture2D<float> binding accept either view, but WebGPU strictly matches
// the bind group's declared sample type against the bound view's format. These can't just be two
// entry points in the same file either: Slang's parameter reflection enumerates every module-scope
// global regardless of which entry point references it, so a single module declaring both
// `source` and `depthSource` reports both bindings for every entry point compiled from it,
// re-creating the same mismatch this split is meant to avoid.
//
// See hiz_build.slang for the max-reduction rationale (depth_compare = Less convention).

import sturdy_common;

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct HiZBuildConstants {
    uint2 sourceExtent;
};

#ifdef SFT_EMULATE_PUSH_CONSTANTS
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

DepthTexture2D depthSource;

[shader("fragment")]
float4 reduceFromDepthMain(VertexOutput input) : SV_Target {
    int2 destPixel = int2(input.position.xy);
    int2 srcBase = destPixel * 2;
    int2 maxSrc = int2(hiz.sourceExtent) - int2(1, 1);
    int2 c0 = min(srcBase, maxSrc);
    int2 c1 = min(srcBase + int2(1, 0), maxSrc);
    int2 c2 = min(srcBase + int2(0, 1), maxSrc);
    int2 c3 = min(srcBase + int2(1, 1), maxSrc);
    float d = max(max(depthSource.Load(int3(c0, 0)), depthSource.Load(int3(c1, 0))),
                  max(depthSource.Load(int3(c2, 0)), depthSource.Load(int3(c3, 0))));
    return float4(d, 0.0, 0.0, 0.0);
}
