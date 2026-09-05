// FrostBite-style motion blur, pass 2 of 3: neighborhood dilation of the per-tile max velocity.
//
// motion_blur_gather.slang (pass 3) walks a line through sceneColor centered on each pixel's blur
// vector; without dilation a fast-moving object's blur would stop exactly at its own tile boundary,
// producing a hard seam against the static tiles next to it. Taking the max-magnitude velocity over
// each tile's own 3x3 neighborhood (again compared by dot(v,v), matching pass 1's convention) lets
// that motion "bleed" one tile's radius into its static surroundings, which is what makes a fast
// object crossing a static background blur convincingly across the seam.

import sturdy_common;

struct NeighborMaxConstants {
    uint2 tileExtent;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
// WebGPU has no push constants at all, so on that target the same block is carried as an
// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of
// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant
// block is packed std430-style, a uniform buffer std140, so a struct whose members would
// pad differently between the two must be laid out to agree under both.
[[vk::binding(0, 3)]] ConstantBuffer<NeighborMaxConstants> neighborMax;
#else
[[push_constant]] ConstantBuffer<NeighborMaxConstants> neighborMax;
#endif
Texture2D<float2> tileMaxVelocity;
// No [format(...)] here deliberately: WGSL's storage-texture format list has no 2-component option
// at all (confirmed via slangc -target wgsl, which silently substitutes rgba32float and warns
// E31105) -- annotating "rg16f" would make Slang's own codegen and this engine's RHI reflection
// disagree about the real format, a *worse* failure mode than the current clean "no reflected
// format" RHI rejection. Fixing this for real needs the backing RHI::Format changed to a
// WGSL-legal 4-channel one (e.g. RGBA16Float) at the texture's creation site, which also means
// updating every shader that reads it as Texture2D<float2> -- tracked as follow-up, not done here.
WTexture2D<float2> dilatedVelocityOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void neighborMaxMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint2 tile = dispatchThreadId.xy;
    if (tile.x >= neighborMax.tileExtent.x || tile.y >= neighborMax.tileExtent.y) {
        return;
    }

    float2 best = float2(0.0, 0.0);
    float bestMagnitudeSq = -1.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int2 sampleTile = clamp(int2(tile) + int2(dx, dy), int2(0, 0),
                                    int2(neighborMax.tileExtent) - int2(1, 1));
            float2 candidate = tileMaxVelocity.Load(int3(sampleTile, 0));
            float magnitudeSq = dot(candidate, candidate);
            if (magnitudeSq > bestMagnitudeSq) {
                bestMagnitudeSq = magnitudeSq;
                best = candidate;
            }
        }
    }
    dilatedVelocityOut.Store(tile, best);
}
