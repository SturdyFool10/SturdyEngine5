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
[[push_constant]] ConstantBuffer<NeighborMaxConstants> neighborMax;

Texture2D<float2> tileMaxVelocity;
RWTexture2D<float2> dilatedVelocityOut;

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
    dilatedVelocityOut[tile] = best;
}
