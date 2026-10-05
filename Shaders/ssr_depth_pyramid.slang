// Screen-space reflections, pass 1 of 4: the min-depth pyramid, all levels in one dispatch.
//
// Each 16x16 group reduces a 64x64 full-resolution tile: every thread takes a 4x4 block down to a 2x2 of level 0 and one
// texel of level 1, then the group halves its shared tile four more times (levels 2..5). No global synchronisation is
// needed because no level coarser than the group's tile is built -- the idea of AMD's single-pass downsampler, stopped
// where SSR traversal stops gaining (a level-5 cell spans 64 screen pixels). Min (nearest) depth, because the trace must
// never step over geometry; this engine's depth is 0 at the near plane.
import sturdy_common;
import ssr_common;

struct PyramidConstants {
    uint2 fullExtent;
    uint2 halfExtent;
};
#ifdef SFT_EMULATE_PUSH_CONSTANTS
[[vk::binding(0, 3)]] ConstantBuffer<PyramidConstants> pyramid;
#else
[[push_constant]] ConstantBuffer<PyramidConstants> pyramid;
#endif

DepthTexture2D sceneDepth;
[format("r32f")]
WTexture2D<float> depthPyramidOut;

groupshared float tile[16][16];

float loadDepth(int2 pixel) {
    if (any(pixel >= int2(pyramid.fullExtent))) {
        return 1.0; // outside the image: as far as possible, never occludes
    }
    return sceneDepth.Load(int3(pixel, 0));
}

void storeLevel(uint level, uint2 texel, float value) {
    if (all(texel < ssrLevelExtent(pyramid.halfExtent, level))) {
        depthPyramidOut.Store(int2(ssrLevelOrigin(pyramid.halfExtent, level) + texel), value);
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void pyramidMain(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    // Level 0: this thread's 2x2 of half-resolution texels, each the min of a 2x2 full-resolution block.
    uint2 level0Base = groupId.xy * 32u + localId.xy * 2u;
    float level1 = 1.0;
    [unroll]
    for (uint y = 0; y < 2; ++y) {
        [unroll]
        for (uint x = 0; x < 2; ++x) {
            uint2 texel = level0Base + uint2(x, y);
            int2 pixel = int2(texel * 2u);
            float value = min(min(loadDepth(pixel), loadDepth(pixel + int2(1, 0))),
                              min(loadDepth(pixel + int2(0, 1)), loadDepth(pixel + int2(1, 1))));
            storeLevel(0u, texel, value);
            level1 = min(level1, value);
        }
    }
    storeLevel(1u, groupId.xy * 16u + localId.xy, level1);
    tile[localId.y][localId.x] = level1;
    GroupMemoryBarrierWithGroupSync();

    // Levels 2..5 from the shared tile, halving the active threads each time.
    uint width = 8u;
    [unroll]
    for (uint level = 2u; level < SSR_PYRAMID_LEVELS; ++level) {
        float value = 1.0;
        bool active = all(localId.xy < uint2(width, width));
        if (active) {
            uint2 source = localId.xy * 2u;
            value = min(min(tile[source.y][source.x], tile[source.y][source.x + 1u]),
                        min(tile[source.y + 1u][source.x], tile[source.y + 1u][source.x + 1u]));
        }
        GroupMemoryBarrierWithGroupSync();
        if (active) {
            tile[localId.y][localId.x] = value;
            storeLevel(level, groupId.xy * width + localId.xy, value);
        }
        GroupMemoryBarrierWithGroupSync();
        width /= 2u;
    }
}
