// Shared by ssr_{depth_pyramid,trace}.slang. `SsrConstants` mirrors `SsrConstants` in
// Renderer/Reflections.cpp; change both sides together.
//
// The min-depth pyramid is one 2D atlas: level 0 (half resolution, min of each 2x2 full-resolution block) at the origin,
// levels 1..SSR_PYRAMID_LEVELS-1 stacked in a column to its right. One storage view, so a single dispatch writes every
// level and every backend binds it the same way.
import sturdy_common;
import sturdy_lighting_data;

static const uint SSR_PYRAMID_LEVELS = 6;

struct SsrConstants {
    // full width, full height, half width, half height
    uint4 extents;
    // max steps, colour mip levels, 1 = lit-colour history valid, unused
    uint4 params;
    // max perceptual roughness, thickness (fraction of view depth), glossy blur scale, unused
    float4 tuning;
};

uint2 ssrLevelExtent(uint2 halfExtent, uint level) {
    uint scale = 1u << level;
    return max((halfExtent + scale - 1u) / scale, uint2(1, 1));
}

uint2 ssrLevelOrigin(uint2 halfExtent, uint level) {
    if (level == 0u) {
        return uint2(0, 0);
    }
    uint y = 0;
    for (uint k = 1; k < level; ++k) {
        y += ssrLevelExtent(halfExtent, k).y;
    }
    return uint2(halfExtent.x, y);
}

float ssrLoadPyramid(Texture2D<float> pyramid, uint2 halfExtent, uint level, int2 texel) {
    int2 extent = int2(ssrLevelExtent(halfExtent, level));
    texel = clamp(texel, int2(0, 0), extent - 1);
    return pyramid.Load(int3(int2(ssrLevelOrigin(halfExtent, level)) + texel, 0));
}

float3 ssrWorldFromDepth(ShadowLightingGpuData data, float2 uv, float deviceDepth) {
    float4 h = mul(float4(uvToNdc(uv), deviceDepth, 1.0), data.inverseViewProjection);
    return h.xyz / (abs(h.w) > SFT_EPSILON ? h.w : SFT_EPSILON);
}

float ssrViewDepth(ShadowLightingGpuData data, float3 world) {
    return abs(mul(float4(world, 1.0), data.view).z);
}
