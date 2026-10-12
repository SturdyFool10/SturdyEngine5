// Lifetimes of `Engine::DebugDraw`: this-frame-only shapes disappear at the next update, timed shapes survive exactly as long as
// asked (in unscaled seconds), disabling drops everything, and the snapshot the renderer takes is stable between changes.

#include <Engine/DebugDraw.hpp>

#include <cstdio>

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    const glm::vec4 red{1.0f, 0.0f, 0.0f, 1.0f};
} // namespace

int main() {
    using SFT::Engine::DebugDraw;

    // Default lifetime: until the next update.
    {
        DebugDraw draw;
        draw.line({0, 0, 0}, {1, 0, 0}, red);
        check(draw.line_count() == 1, "a drawn line is alive");
        const auto first = draw.snapshot();
        check(first && first->size() == 1, "the snapshot holds it");
        check(draw.snapshot() == first, "an unchanged list returns the same snapshot (no copy per surface)");
        draw.advance(1.0 / 60.0);
        check(draw.line_count() == 0 && draw.snapshot() == nullptr, "a this-frame line is gone after the next update");
        check(first->size() == 1, "a snapshot already taken stays valid after the shapes expire");
    }

    // Timed shapes: alive until their lifetime has elapsed, regardless of how time is chopped up.
    {
        DebugDraw draw;
        draw.sphere({0, 0, 0}, 1.0f, red, 0.5f);
        const usize segments = draw.line_count();
        check(segments > 0, "a sphere records segments");
        draw.advance(0.2);
        draw.advance(0.2);
        check(draw.line_count() == segments, "a 0.5 s shape survives 0.4 s");
        draw.line({0, 0, 0}, {0, 1, 0}, red); // this frame only
        draw.advance(0.2);
        check(draw.line_count() == 0, "...and is gone at 0.6 s along with the frame-only line");
    }

    // Lifetimes are independent per shape.
    {
        DebugDraw draw;
        draw.cross({0, 0, 0}, 1.0f, red, 1.0f);
        draw.cross({5, 0, 0}, 1.0f, red, 3.0f);
        draw.advance(1.5);
        check(draw.line_count() == 3, "only the 1 s cross has expired at 1.5 s");
        draw.advance(2.0);
        check(draw.line_count() == 0, "both are gone at 3.5 s");
    }

    // Disabled: calls are ignored, existing shapes are dropped, nothing is snapshotted.
    {
        DebugDraw draw;
        draw.line({0, 0, 0}, {1, 1, 1}, red, 10.0f);
        draw.set_enabled(false);
        check(draw.line_count() == 0 && !draw.enabled(), "disabling drops everything");
        draw.line({0, 0, 0}, {1, 1, 1}, red);
        check(draw.line_count() == 0 && draw.snapshot() == nullptr, "a disabled DebugDraw ignores calls");
        draw.set_enabled(true);
        draw.line({0, 0, 0}, {1, 1, 1}, red);
        check(draw.line_count() == 1, "re-enabling accepts shapes again");
    }

    // Width applies to what is drawn after it; submit() takes a list recorded elsewhere; clear() empties.
    {
        DebugDraw draw;
        draw.set_width(5.0f);
        draw.line({0, 0, 0}, {1, 0, 0}, red);
        SFT::Renderer::DebugDrawList list;
        list.set_width(2.0f);
        list.box({0, 0, 0}, {1, 1, 1}, red);
        draw.submit(list, 1.0f);
        const auto snapshot = draw.snapshot();
        check(snapshot && snapshot->size() == 13, "a line and a submitted box are 13 segments");
        check(snapshot && (*snapshot)[0].width_px == 5.0f && (*snapshot)[1].width_px == 2.0f, "each keeps the width it was recorded with");
        draw.clear();
        check(draw.line_count() == 0, "clear() empties the list");
    }

    // A runaway caller cannot grow the backlog without bound.
    {
        DebugDraw draw;
        SFT::Renderer::DebugDrawSettings settings;
        settings.max_lines = 100;
        draw.set_settings(settings);
        for (int i = 0; i < 1000; ++i) draw.line({0, 0, 0}, {1, 0, 0}, red, 60.0f);
        check(draw.line_count() <= 200, "the backlog is capped at twice max_lines");
    }

    if (failures == 0) std::printf("DebugDrawLifetimeTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
