// The CxxApi debug-draw surface over a standalone `Engine::DebugDraw`: every function records what its name says.

#include <CxxApi/DebugDraw.hpp>

#include <cstdio>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    constexpr std::array<float, 4> kRed{1.0f, 0.0f, 0.0f, 1.0f};
    constexpr std::array<float, 3> kO{0.0f, 0.0f, 0.0f};
    constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
} // namespace

int main() {
    Engine::DebugDraw draw;

    debug_line(draw, kO, {1, 0, 0}, kRed, 0.0f);
    check(debug_line_count(draw) == 1, "a line is one segment");
    debug_box(draw, kO, {1, 1, 1}, kRed, 0.0f);
    check(debug_line_count(draw) == 13, "a box adds 12");
    debug_cross(draw, kO, 1.0f, kRed, 0.0f);
    check(debug_line_count(draw) == 16, "a cross adds 3");

    debug_clear(draw);
    check(debug_line_count(draw) == 0, "clear empties it");

    // A flat float vector becomes xyz triples; the incomplete tail is ignored.
    debug_polyline(draw, {0, 0, 0, 1, 0, 0, 1, 1, 0, 9.0f}, kRed, false, 0.0f);
    check(debug_line_count(draw) == 2, "3 points make 2 segments (trailing float ignored)");
    debug_clear(draw);
    debug_polyline(draw, {0, 0, 0, 1, 0, 0, 1, 1, 0}, kRed, true, 0.0f);
    check(debug_line_count(draw) == 3, "a closed triangle has 3 segments");
    debug_clear(draw);

    debug_oriented_box(draw, kIdentity, {1, 1, 1}, kRed, 0.0f);
    debug_sphere(draw, kO, 1.0f, kRed, 0.0f);
    debug_circle(draw, kO, {0, 1, 0}, 1.0f, kRed, 0.0f);
    debug_capsule(draw, kO, {0, 2, 0}, 0.5f, kRed, 0.0f);
    debug_cone(draw, kO, {0, -1, 0}, 2.0f, 0.4f, kRed, 0.0f);
    debug_arrow(draw, kO, {0, 0, 3}, kRed, 0.0f, 0.0f);
    debug_axes(draw, kIdentity, 1.0f, 0.0f);
    debug_frustum(draw, kIdentity, kRed, 0.0f);
    debug_grid(draw, kO, {1, 0, 0}, {0, 0, 1}, 2, 1.0f, kRed, 0.0f);
    check(debug_line_count(draw) > 100, "every shape function records segments");

    // Width and occlusion options reach the engine object; the master switch stops recording.
    debug_set_width(draw, 3.0f);
    debug_set_occlusion(draw, false, 5.0f);
    const Renderer::DebugDrawSettings settings = draw.settings();
    check(!settings.depth_test && settings.occluded_alpha == 1.0f, "occlusion options are applied (alpha clamped to 1)");
    debug_clear(draw);
    debug_line(draw, kO, {1, 1, 1}, kRed, 0.0f);
    check(draw.snapshot() && (*draw.snapshot())[0].width_px == 3.0f, "the width applies to later shapes");

    debug_set_enabled(draw, false);
    debug_line(draw, kO, {1, 1, 1}, kRed, 0.0f);
    check(!debug_enabled(draw) && debug_line_count(draw) == 0, "disabled: nothing is recorded");

    if (failures == 0) std::printf("CxxApiDebugDrawTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
