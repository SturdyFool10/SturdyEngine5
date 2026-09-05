// GTAO pass 1: hardware depth -> linear view-space depth, plus a 5-level depth mip hierarchy.
//
// Two reasons this exists rather than the main pass reading the depth buffer directly:
//
//  * Nonlinear device depth is useless for horizon math. Every tap would have to be linearized
//    anyway, and doing it once per texel instead of once per tap is strictly cheaper.
//  * Far taps along a slice can read a coarser mip. The main pass's cost is dominated by texture
//    cache behaviour, not arithmetic, and a tap 60 pixels away pulling from mip 3 touches a cache
//    line that its neighbouring pixels already pulled in.
//
// The reduction is depth-aware (see gtaoDepthMipFilter) so mip texels never straddle a silhouette.
//
// One thread produces a 2x2 block of mip 0, so an 8x8 group covers 16x16 pixels and can reduce all
// the way down to a single mip-4 texel using only group-shared memory.
import sturdy_common;
import gtao_common;

ConstantBuffer<GtaoConstants> gtao;
// DepthTexture2D (not Texture2D<float>): this is always bound to the real resolved depth buffer's
// view (Depth32Float, sample type Depth). WebGPU strictly matches a bind group's declared sample
// type against the bound view's format, unlike Vulkan/D3D12 where a Float-typed binding harmlessly
// accepts either. Depth textures support textureLoad in every shader stage including compute, so
// this is a pure type-widening fix with no behavior change on any target.
DepthTexture2D sceneDepth;
// WGSL's texture_storage_2d requires a format in the type itself; must match the RHI::Format the
// depth-pyramid mips are actually created with (RHI::Format::R32Float). Write-only: this pass never
// reads back its own output.
[format("r32f")]
WTexture2D<float> viewDepthMip0;
[format("r32f")]
WTexture2D<float> viewDepthMip1;
[format("r32f")]
WTexture2D<float> viewDepthMip2;
[format("r32f")]
WTexture2D<float> viewDepthMip3;
[format("r32f")]
WTexture2D<float> viewDepthMip4;

groupshared float gtaoSharedDepths[8][8];

float loadLinearDepth(int2 pixel, int2 maxPixel) {
    // Clamped rather than bounds-rejected: an out-of-range tap must behave like the nearest real
    // surface, never like depth 0 (which linearizes to the near plane and would fabricate a
    // foreground occluder along the right/bottom edges of odd-sized render targets).
    int2 clamped = clamp(pixel, int2(0, 0), maxPixel);
    return gtaoLinearizeDepth(sceneDepth.Load(int3(clamped, 0)), gtao);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void prefilterDepthMain(uint3 dispatchThreadId : SV_DispatchThreadID,
                        uint3 groupThreadId : SV_GroupThreadID,
                        uint3 groupId : SV_GroupID) {
    int2 extent = int2(gtao.viewportSizePixelSize.xy);
    int2 maxPixel = extent - int2(1, 1);

    // Mip 0: this thread's own 2x2 block.
    int2 base = int2(groupId.xy) * 16 + int2(groupThreadId.xy) * 2;
    float d00 = loadLinearDepth(base + int2(0, 0), maxPixel);
    float d10 = loadLinearDepth(base + int2(1, 0), maxPixel);
    float d01 = loadLinearDepth(base + int2(0, 1), maxPixel);
    float d11 = loadLinearDepth(base + int2(1, 1), maxPixel);

    // Writes outside the image are discarded by the API, so the 2x2 granularity costs nothing on
    // odd-sized targets.
    viewDepthMip0.Store(base + int2(0, 0), d00);
    viewDepthMip0.Store(base + int2(1, 0), d10);
    viewDepthMip0.Store(base + int2(0, 1), d01);
    viewDepthMip0.Store(base + int2(1, 1), d11);

    float mip1 = gtaoDepthMipFilter(d00, d10, d01, d11, gtao);
    viewDepthMip1.Store(int2(groupId.xy) * 8 + int2(groupThreadId.xy), mip1);
    gtaoSharedDepths[groupThreadId.y][groupThreadId.x] = mip1;

    // Mips 2..4 are pure group-shared reductions: each step keeps its result at the same slot the
    // top-left contributor occupies, so the next step reads at twice the stride.
    GroupMemoryBarrierWithGroupSync();
    if (all((groupThreadId.xy % 2) == 0)) {
        uint x = groupThreadId.x;
        uint y = groupThreadId.y;
        float value = gtaoDepthMipFilter(gtaoSharedDepths[y][x], gtaoSharedDepths[y][x + 1],
                                         gtaoSharedDepths[y + 1][x], gtaoSharedDepths[y + 1][x + 1], gtao);
        viewDepthMip2.Store(int2(groupId.xy) * 4 + int2(groupThreadId.xy) / 2, value);
        gtaoSharedDepths[y][x] = value;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all((groupThreadId.xy % 4) == 0)) {
        uint x = groupThreadId.x;
        uint y = groupThreadId.y;
        float value = gtaoDepthMipFilter(gtaoSharedDepths[y][x], gtaoSharedDepths[y][x + 2],
                                         gtaoSharedDepths[y + 2][x], gtaoSharedDepths[y + 2][x + 2], gtao);
        viewDepthMip3.Store(int2(groupId.xy) * 2 + int2(groupThreadId.xy) / 4, value);
        gtaoSharedDepths[y][x] = value;
    }

    GroupMemoryBarrierWithGroupSync();
    if (all((groupThreadId.xy % 8) == 0)) {
        float value = gtaoDepthMipFilter(gtaoSharedDepths[0][0], gtaoSharedDepths[0][4],
                                         gtaoSharedDepths[4][0], gtaoSharedDepths[4][4], gtao);
        viewDepthMip4.Store(int2(groupId.xy), value);
    }
}
