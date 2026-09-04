// FrostBite-style motion blur, pass 1 of 3: per-tile max-magnitude velocity reduction.
//
// Reduces gbuffer_geometry.slang's per-pixel motion vector (SV_Target4, camera-space NDC delta,
// halved — see that shader's own doc comment) down to one representative velocity per tileSize x
// tileSize screen tile, keeping whichever pixel in the tile has the largest magnitude rather than
// an average: a single fast-moving object crossing a mostly-static tile must still blur that whole
// tile's neighborhood (motion_blur_neighbor_max.slang, pass 2) by its full extent, which an average
// would dilute away. Magnitude compares by squared length (dot(v,v)) — monotonic with the true
// length and avoids a sqrt per candidate.
//
// One thread group per output tile. Threads are laid out on a fixed 32x32 grid (the largest tile
// size this pass supports) regardless of the runtime tileSize push constant, since Slang/HLSL
// numthreads must be a compile-time constant; threads outside the actual tileSize x tileSize
// footprint contribute a zero velocity to the reduction and never influence the max.

import sturdy_common;

static const uint kMaxTileSize = 32;
static const uint kMaxTileAreaThreads = kMaxTileSize * kMaxTileSize;

struct TileMaxConstants {
    uint2 renderExtent;
    uint tileSize;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
// WebGPU has no push constants at all, so on that target the same block is carried as an
// ordinary uniform buffer in the reserved set the WebGPU backend binds a per-draw slice of
// (see WebGpuCommandEncoder.cpp). Note the layout rule changes with it: a push-constant
// block is packed std430-style, a uniform buffer std140, so a struct whose members would
// pad differently between the two must be laid out to agree under both.
[[vk::binding(0, 3)]] ConstantBuffer<TileMaxConstants> tileMax;
#else
[[push_constant]] ConstantBuffer<TileMaxConstants> tileMax;
#endif
Texture2D<float2> gbufferMotion;
RWTexture2D<float2> tileMaxVelocityOut;

groupshared float2 sharedVelocity[kMaxTileAreaThreads];

[shader("compute")]
[numthreads(kMaxTileSize, kMaxTileSize, 1)]
void tileMaxMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    uint2 localId = groupThreadId.xy;
    uint linearIndex = localId.y * kMaxTileSize + localId.x;

    float2 velocity = float2(0.0, 0.0);
    if (localId.x < tileMax.tileSize && localId.y < tileMax.tileSize) {
        uint2 pixel = groupId.xy * tileMax.tileSize + localId;
        if (pixel.x < tileMax.renderExtent.x && pixel.y < tileMax.renderExtent.y) {
            velocity = gbufferMotion.Load(int3(int2(pixel), 0));
        }
    }
    sharedVelocity[linearIndex] = velocity;
    GroupMemoryBarrierWithGroupSync();

    // Standard power-of-two tree reduction over the fixed 1024-thread group — every stride step
    // halves the number of live candidates regardless of how much of the group the real tileSize
    // footprint actually used, since the unused lanes were already zero-filled above.
    [unroll]
    for (uint stride = kMaxTileAreaThreads / 2; stride > 0; stride >>= 1) {
        if (linearIndex < stride) {
            float2 a = sharedVelocity[linearIndex];
            float2 b = sharedVelocity[linearIndex + stride];
            sharedVelocity[linearIndex] = dot(b, b) > dot(a, a) ? b : a;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (linearIndex == 0) {
        tileMaxVelocityOut[groupId.xy] = sharedVelocity[0];
    }
}
