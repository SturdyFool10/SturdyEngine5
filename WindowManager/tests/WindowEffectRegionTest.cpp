#include <WindowManager/WindowEffects.hpp>

#include <cstdio>
#include <set>
#include <utility>

// Explicit checks rather than assert(): asserts compile to nothing under NDEBUG and would make this a no-op.
#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                        \
        }                                                                                      \
    } while (false)

using namespace SFT::WindowManager;

namespace {

    /// The set of covered pixels (x, y) for a list of rectangles.
    std::set<std::pair<int, int>> covered(const std::vector<EffectRect> &rects) {
        std::set<std::pair<int, int>> pixels;
        for (const EffectRect &r : rects) {
            for (int y = r.y; y < r.y + r.height; ++y) {
                for (int x = r.x; x < r.x + r.width; ++x) {
                    pixels.insert({x, y});
                }
            }
        }
        return pixels;
    }

} // namespace

int main() {
    int failures = 0;

    // No rectangles and no rounding: "the whole window", expressed as an empty region.
    {
        WindowEffectOptions options;
        options.window_width = 200;
        options.window_height = 100;
        CHECK(resolve_blur_region(options).empty());
    }

    // Explicit rectangles are passed through untouched when square.
    {
        WindowEffectOptions options;
        options.blur_rects[0] = EffectRect{10, 20, 30, 40};
        options.blur_rect_count = 1;
        const auto region = resolve_blur_region(options);
        CHECK(region.size() == 1 && region[0].x == 10 && region[0].y == 20 && region[0].width == 30 && region[0].height == 40);
    }

    // Rounded corners cut the corner pixels, keep the centre and the straight edges, and stay symmetric.
    {
        WindowEffectOptions options;
        options.window_width = 100;
        options.window_height = 60;
        options.corner_radius = 12.0f;
        const auto region = resolve_blur_region(options);
        const auto pixels = covered(region);
        CHECK(!region.empty());
        CHECK(pixels.count({0, 0}) == 0);   // the very corner is outside the arc
        CHECK(pixels.count({99, 0}) == 0);
        CHECK(pixels.count({0, 59}) == 0);
        CHECK(pixels.count({99, 59}) == 0);
        CHECK(pixels.count({50, 0}) == 1);  // top edge, away from the corner
        CHECK(pixels.count({0, 30}) == 1);  // left edge, away from the corner
        CHECK(pixels.count({50, 30}) == 1);
        // Mirror symmetry in both axes, and nothing outside the window.
        bool symmetric = true;
        bool inside = true;
        for (const auto &[x, y] : pixels) {
            symmetric = symmetric && pixels.count({99 - x, y}) == 1 && pixels.count({x, 59 - y}) == 1;
            inside = inside && x >= 0 && x < 100 && y >= 0 && y < 60;
        }
        CHECK(symmetric);
        CHECK(inside);
        // A radius is a handful of rectangles, not one per pixel.
        CHECK(region.size() <= 2 * 12 + 1);
        // Area is just under the full rectangle: the four corners lose roughly (4 - pi) * r^2.
        const double full = 100.0 * 60.0;
        const double lost = full - static_cast<double>(pixels.size());
        CHECK(lost > 0.5 * (4.0 - 3.14159) * 144.0 && lost < 2.0 * (4.0 - 3.14159) * 144.0);
    }

    // A radius larger than half the window is clamped instead of producing negative sizes.
    {
        WindowEffectOptions options;
        options.window_width = 40;
        options.window_height = 40;
        options.corner_radius = 500.0f;
        const auto region = resolve_blur_region(options);
        bool valid = !region.empty();
        for (const EffectRect &r : region) {
            valid = valid && r.width > 0 && r.height > 0;
        }
        CHECK(valid);
    }

    // Rounding with an unknown window size falls back to the whole window rather than guessing.
    {
        WindowEffectOptions options;
        options.corner_radius = 8.0f;
        CHECK(resolve_blur_region(options).empty());
    }

    // Support aggregation: any available route makes the effect available; unknown beats unavailable.
    {
        WindowEffectSupport support;
        CHECK(support.availability == EffectAvailability::Unavailable);
        support.add("a", EffectAvailability::Unavailable, "no");
        CHECK(support.availability == EffectAvailability::Unavailable);
        support.add("b", EffectAvailability::Unknown, "maybe");
        CHECK(support.availability == EffectAvailability::Unknown);
        support.add("c", EffectAvailability::Available);
        CHECK(support.availability == EffectAvailability::Available);
        support.add("d", EffectAvailability::Unavailable, "no");
        CHECK(support.availability == EffectAvailability::Available && support.route_count == 4);
        support.add("overflow", EffectAvailability::Unavailable);
        CHECK(support.route_count == WindowEffectSupport::max_routes);
    }

    if (failures == 0) {
        std::puts("WindowEffectRegionTest: all checks passed");
    }
    return failures == 0 ? 0 : 1;
}
