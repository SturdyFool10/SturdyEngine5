// The CxxApi window surface on a bare engine (no window, no graphics device): defaults match the engine's, absence is
// reported in the return value where the header says so, and every fallible call throws `Error` rather than crashing.

#include <CxxApi/CxxApi.hpp>
#include <CxxApi/Error.hpp>

#include <cstdio>
#include <string>
#include <vector>

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

    template <typename F> bool throws_error(F &&call) {
        try {
            call();
        } catch (const Error &) {
            return true;
        }
        return false;
    }
} // namespace

int main() {
    const WindowDesc desc = window_desc_defaults();
    check(desc.width == 1280 && desc.height == 720, "default window is 1280x720");
    check(desc.visible && desc.resizable && desc.decorated && desc.high_dpi, "default window is visible, resizable, decorated, high-dpi");
    check(!desc.has_position && !desc.transparent && desc.mode == WindowMode::Windowed, "default window has no fixed position");

    const SceneLightingDesc lighting = scene_lighting_defaults();
    check(lighting.exposure == 1.0f && lighting.ambient_radiance[0] == 0.02f, "default scene lighting matches the engine");

    Engine::Engine engine;
    check(!window_has_primary(engine), "a bare engine has no primary window");
    check(!window_close_requested(engine), "no window means no close request");
    check(window_count(engine) == 0, "no managed windows");
    check(window_content_scale(engine) == 1.0f, "content scale falls back to 1");
    check(window_fullscreen_mode(engine) == WindowMode::Windowed, "fullscreen mode falls back to windowed");

    check(!window_position(engine).ok && !window_size(engine).ok && !window_framebuffer_size(engine).ok &&
              !window_global_cursor_position(engine).ok,
          "geometry queries report absence instead of throwing");
    check(!window_hdr_properties(engine).available, "no HDR properties without a window");

    WindowSnapshot snapshot{};
    check(!window_primary_snapshot(engine, snapshot), "no primary snapshot");
    check(!window_find(engine, 1, snapshot), "unknown window id is not found");

    check(throws_error([&] { window_show(engine); }), "show without a window throws");
    check(throws_error([&] { window_set_title(engine, "x"); }), "set_title without a window throws");
    check(throws_error([&] { (void)window_opacity(engine); }), "opacity without a window throws");
    check(throws_error([&] { (void)window_refresh_rate_hz(engine); }), "refresh rate without a window throws");
    check(throws_error([&] { (void)window_clipboard_text(engine); }), "clipboard without a window throws");
    check(throws_error([&] { (void)window_snapshot_at(engine, 0); }), "snapshot index out of range throws");
    check(throws_error([&] { (void)window_request_spawn(engine, desc, "child"); }), "spawn without a graphics device throws");

    // Queued requests need no device and complete asynchronously, so this only checks the queue round trip.
    const std::uint64_t close_id = window_request_close_window(engine, 7);
    window_request_cursor_icon(engine, 7, CursorIcon::Pointer);
    window_request_effect(engine, 7, WindowEffect::Mica, true);
    WindowEffectOptions opacity_options;
    opacity_options.opacity = 0.5f;
    window_request_effect_ex(engine, 7, WindowEffect::Opacity, true, opacity_options);
    // The union surface: queued operations leave no status until a window runs them, and an unknown window has no
    // features or geometry rather than a crash.
    window_queue_set_always_on_top(engine, 7, true);
    window_queue_query_clipboard_text(engine, 7);
    check(!window_operation_status(engine, 7, WindowOperation::AlwaysOnTop).completed, "a queued operation has not run yet");
    check(!window_feature_supported(engine, 7, WindowFeature::AlwaysOnTop), "an unknown window supports nothing");
    check(window_geometry(engine, 7).safe_width == 0, "an unknown window has no geometry");
    check(throws_error([&] { window_queue_set_parent(engine, 7, true, 99, false); }), "parenting to a window that does not exist throws");
    window_request_text_input_area(engine, 7, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    check(close_id != 0, "a queued close returns a request id");

    std::vector<std::string> messages{"stale"};
    const auto completions = window_take_completions(engine, messages);
    check(completions->size() == messages.size(), "completions and messages stay index-aligned");

    if (failures != 0) {
        std::fprintf(stderr, "CxxApiWindowTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CxxApiWindowTest: ok\n");
    return 0;
}
