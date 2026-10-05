// CPU-only checks of the reflection planning helpers (Renderer/Reflections.cpp): which environment levels refresh in a
// frame, and the min-depth pyramid atlas size the SSR shaders (Shaders/ssr_common.slang) index into.

#include <Renderer/Reflections.hpp>

#include <cstdio>

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    // The shader's level extent / origin, reimplemented independently.
    unsigned level_extent(unsigned half, unsigned level) {
        const unsigned v = (half + (1u << level) - 1u) >> level;
        return v < 1u ? 1u : v;
    }
} // namespace

int main() {
    using namespace SFT::Renderer;

    // An empty atlas is filled completely.
    check(reflection_environment_level_mask(0, false) == 0x3Fu, "an empty atlas refreshes every level");
    check(reflection_environment_level_mask(123456, false) == 0x3Fu, "...at any frame");

    // A filled atlas refreshes the mirror level plus exactly one rough level, cycling through all of them.
    unsigned seen = 0;
    bool always_mirror = true, one_rough = true;
    for (unsigned long long frame = 0; frame < 5; ++frame) {
        const unsigned mask = reflection_environment_level_mask(frame, true);
        always_mirror = always_mirror && (mask & 1u) != 0;
        one_rough = one_rough && __builtin_popcount(mask & ~1u) == 1;
        seen |= mask;
    }
    check(always_mirror, "the mirror level refreshes every frame");
    check(one_rough, "one rough level per frame");
    check(seen == 0x3Fu, "five frames cover every level");

    // Pyramid atlas: level 0 plus a column of coarser levels must fit, for odd and tiny sizes too.
    const glm::uvec2 sizes[] = {{960, 540}, {641, 361}, {7, 5}, {1, 1}, {1920, 1080}};
    for (const glm::uvec2 half : sizes) {
        const glm::uvec2 atlas = reflection_pyramid_extent(half);
        unsigned column = 0;
        for (unsigned level = 1; level < 6; ++level) column += level_extent(half.y, level);
        check(atlas.x == half.x + level_extent(half.x, 1), "atlas width = level 0 + the column");
        check(atlas.y >= half.y && atlas.y >= column, "atlas height holds level 0 and the stacked column");
    }

    // The colour chain halves (rounding up) from the half-resolution extent and never reaches zero.
    check(reflection_color_level_extent({960, 540}, 0) == glm::uvec2(960, 540), "chain level 0 is the half-resolution extent");
    check(reflection_color_level_extent({960, 540}, 1) == glm::uvec2(480, 270), "chain level 1 halves it");
    check(reflection_color_level_extent({641, 361}, 4) == glm::uvec2(41, 23), "odd sizes round up (641/16, 361/16)");
    check(reflection_color_level_extent({7, 5}, 4) == glm::uvec2(1, 1), "a tiny chain bottoms out at one texel");

    if (failures == 0) std::printf("ReflectionsPlanTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
