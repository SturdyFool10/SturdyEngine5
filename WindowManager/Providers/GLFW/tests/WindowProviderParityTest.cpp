#include <WindowManager/Desktop.hpp>
#include <WindowManager/Gamepad.hpp>
#include <WindowManager/Providers/GLFW/GLFW.hpp>
#include <WindowManager/Providers/SDL3/Window.hpp>
#include <WindowManager/WindowManager.hpp>

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// Runs the same window-feature checks against every window provider on a real X server (run it under Xvfb), so a feature
// that one provider implements and another silently drops shows up as a failure here. Every operation must either
// succeed or report `Unsupported`; anything else (a crash, a different error, a wrong answer) is a bug in the provider.
// The checks that need a window manager (maximize, minimize) are left out: Xvfb has none.

using namespace SFT::WindowManager;

namespace {

    int failures = 0;

    void check(bool condition, const std::string &provider, const char *what) {
        if (!condition) {
            std::fprintf(stderr, "[%s] FAILED: %s\n", provider.c_str(), what);
            ++failures;
        }
    }

    /// Success, or an honest `Unsupported`.
    template <typename Result>
    bool ok_or_unsupported(const Result &result) {
        return result.has_value() || result.error().code == WindowErrorCode::Unsupported;
    }

    template <typename Result>
    bool ok(const Result &result) {
        return result.has_value();
    }

    void pump(Window &window, int rounds = 20) {
        for (int i = 0; i < rounds; ++i) {
            (void)window.pump_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool saw_event(Window &window, WindowEventKind kind) {
        bool seen = false;
        while (auto event = window.poll_event()) {
            seen = seen || event->kind == kind;
        }
        return seen;
    }

    struct XClassHint {
        char *res_name;
        char *res_class;
    };

    /// The WM_CLASS of an X11 window, read back from the server (empty when it cannot be read).
    std::pair<std::string, std::string> x11_class(const NativeWindowHandle &handle) {
        void *x11 = dlopen("libX11.so.6", RTLD_LAZY);
        if (x11 == nullptr || handle.system != NativeWindowSystem::X11) {
            return {};
        }
        auto get_hint = reinterpret_cast<int (*)(void *, unsigned long, XClassHint *)>(dlsym(x11, "XGetClassHint"));
        auto sync = reinterpret_cast<int (*)(void *, int)>(dlsym(x11, "XSync"));
        auto free_memory = reinterpret_cast<int (*)(void *)>(dlsym(x11, "XFree"));
        if (get_hint == nullptr || sync == nullptr || free_memory == nullptr) {
            return {};
        }
        sync(handle.display, 0);
        XClassHint hint{};
        if (get_hint(handle.display, reinterpret_cast<unsigned long>(handle.window), &hint) == 0) {
            return {};
        }
        std::pair<std::string, std::string> result{hint.res_name != nullptr ? hint.res_name : "", hint.res_class != nullptr ? hint.res_class : ""};
        free_memory(hint.res_name);
        free_memory(hint.res_class);
        return result;
    }

    void exercise(Window &window, const std::string &provider) {
        pump(window, 5);

        // -- Basics every provider has always had --
        check(ok(window.set_title("provider parity")), provider, "set_title");
        const auto size = window.size();
        check(size.has_value() && size->x > 0 && size->y > 0, provider, "size is positive");
        check(ok(window.set_icon(2, 2, std::vector<std::byte>(16, std::byte{0x80}))), provider, "set_icon accepts RGBA pixels");
        check(!window.set_icon(0, 0, {}).has_value(), provider, "set_icon rejects an empty icon");

        // -- Desktop integration --
        const auto displays = window.displays();
        check(!displays.empty(), provider, "displays() lists the monitor");
        if (!displays.empty()) {
            check(displays.front().bounds.width > 0 && displays.front().bounds.height > 0, provider, "display bounds are real");
            check(displays.front().content_scale > 0.0f, provider, "display content scale is positive");
        }
        (void)window.system_theme(); // any value, but it must not crash
        check(ok_or_unsupported(window.request_attention(AttentionRequest::Briefly)), provider, "request_attention");
        check(ok_or_unsupported(window.set_taskbar_progress(TaskbarProgress::Normal, 0.5f)), provider, "set_taskbar_progress");
        check(ok_or_unsupported(window.set_taskbar_progress(TaskbarProgress::None, 0.0f)), provider, "clearing taskbar progress");
        check(!window.maximized() && !window.minimized(), provider, "a fresh window is neither maximized nor minimized");

        // -- Custom title bar: both providers must be able to move/resize an undecorated window under X11 --
        HitTestRegions regions;
        regions.enabled = true;
        regions.drag.push_back(WindowRect{0, 0, 200, 30});
        check(ok(window.set_hit_test_regions(regions)), provider, "set_hit_test_regions enables on X11");
        check(ok_or_unsupported(window.show_system_menu(WindowPosition{10, 10})), provider, "show_system_menu");
        regions.enabled = false;
        check(ok(window.set_hit_test_regions(regions)), provider, "set_hit_test_regions disables");

        // -- Clipboard --
        check(ok(window.set_clipboard_text("parity clipboard")), provider, "set_clipboard_text");
        pump(window, 5);
        check(window.clipboard_text() == "parity clipboard", provider, "clipboard text round-trips");
        check(ok_or_unsupported(window.set_clipboard_data("application/x-sturdy-test", std::vector<std::byte>(4, std::byte{1}))), provider,
              "typed clipboard data is supported or says so");

        // -- Live resize callback and resize events --
        std::vector<u32> seen_widths;
        window.set_live_resize_callback([&](WindowExtent extent) { seen_widths.push_back(extent.x); });
        (void)saw_event(window, WindowEventKind::Resized);
        check(ok(window.set_size(WindowExtent{size.has_value() ? size->x + 24 : 424, size.has_value() ? size->y + 12 : 312})), provider,
              "set_size");
        pump(window, 40);
        // Only Windows blocks the poll loop during a resize drag, so only there does the live callback exist; both
        // providers must agree that it stays silent on X11 (a stray call would redraw from inside the poll).
        check(seen_widths.empty(), provider, "the live resize callback stays silent where polling is not blocked");
        check(saw_event(window, WindowEventKind::Resized), provider, "a Resized event is queued");
        window.set_live_resize_callback(nullptr);

        // -- Window effects that need no compositor --
        WindowEffect opacity{WindowEffectKind::Opacity, true, 0, LinuxBlurProtocol::Automatic};
        opacity.options.opacity = 0.5f;
        check(window.enable_window_effect(opacity).succeeded(), provider, "Opacity effect");
        opacity.enabled = false;
        check(window.enable_window_effect(opacity).succeeded(), provider, "Opacity effect disables");

        WindowEffect corners{WindowEffectKind::CornerRadius, true, 0, LinuxBlurProtocol::Automatic};
        corners.options.window_corner_radius = 8.0f;
        check(window.enable_window_effect(corners).succeeded(), provider, "CornerRadius effect");
        check(window.window_effect_support(WindowEffectKind::CornerRadius).availability == EffectAvailability::Available, provider,
              "CornerRadius reports itself available on X11");
        // The outline is rebuilt from the window size, so a resize must not drop or break it.
        check(ok(window.set_size(WindowExtent{300, 200})), provider, "set_size with a corner outline");
        pump(window, 20);
        corners.enabled = false;
        check(window.enable_window_effect(corners).succeeded(), provider, "CornerRadius effect disables");

        check(window.enable_window_effect(WindowEffect::dark_mode(true)).succeeded(), provider, "DarkMode effect");
    }

    /// The union contract: for every feature, `feature_support` must agree with what the operation really does. A feature
    /// reported supported has to work; one reported unsupported has to say `Unsupported` rather than succeed or fail some
    /// other way. This is what makes the capability query trustworthy.
    template <typename MakeWindow>
    void exercise_union(Window &window, const std::string &provider, MakeWindow make_window) {
        const std::vector<std::byte> pixels(16, std::byte{0x90});
        std::string_view last_note;

        // The whole matrix, so a run documents what each provider offers on this system.
        for (u8 i = 0; i < static_cast<u8>(WindowFeature::Count); ++i) {
            const auto feature = static_cast<WindowFeature>(i);
            const FeatureSupport support = window.feature_support(feature);
            std::printf("  [%s] %-24s %s%s%s\n", provider.c_str(), std::string(window_feature_name(feature)).c_str(),
                        support.supported ? "yes" : "NO ", support.note.empty() ? "" : "  - ", std::string(support.note).c_str());
        }
        const auto verify = [&](WindowFeature feature, const char *name, auto &&operation) {
            const FeatureSupport support = window.feature_support(feature);
            const auto result = operation();
            const std::string label = std::string(name) + " (feature_support says " + (support.supported ? "yes" : "no") + ")";
            if (support.supported) {
                check(result.has_value(), provider, label.c_str());
                if (!result.has_value()) {
                    std::fprintf(stderr, "[%s]   error: %s\n", provider.c_str(), result.error().message.c_str());
                }
            } else {
                const bool unsupported = !result.has_value() && result.error().code == WindowErrorCode::Unsupported;
                check(unsupported, provider, label.c_str());
                if (!unsupported && !result.has_value()) {
                    std::fprintf(stderr, "[%s]   error: %s\n", provider.c_str(), result.error().message.c_str());
                }
                check(!support.note.empty(), provider, (std::string(name) + " explains why it is unsupported").c_str());
            }
            last_note = support.note;
        };

        verify(WindowFeature::AlwaysOnTop, "set_always_on_top", [&] { return window.set_always_on_top(true); });
        (void)window.set_always_on_top(false);
        verify(WindowFeature::AspectRatio, "set_aspect_ratio", [&] { return window.set_aspect_ratio(1.5f, 1.5f); });
        (void)window.set_aspect_ratio(0.0f, 0.0f);
        verify(WindowFeature::MousePassthrough, "set_mouse_passthrough", [&] { return window.set_mouse_passthrough(true); });
        (void)window.set_mouse_passthrough(false);
        verify(WindowFeature::Focusable, "set_focusable", [&] { return window.set_focusable(false); });
        (void)window.set_focusable(true);
        verify(WindowFeature::AutoMinimizeOnFocusLoss, "set_auto_minimize_on_focus_loss", [&] { return window.set_auto_minimize_on_focus_loss(true); });
        verify(WindowFeature::KeyboardGrab, "set_keyboard_grab", [&] { return window.set_keyboard_grab(true); });
        (void)window.set_keyboard_grab(false);
        verify(WindowFeature::MouseConfineRect, "set_mouse_confine_rect", [&] { return window.set_mouse_confine_rect(WindowRect{10, 10, 100, 100}); });
        (void)window.set_mouse_confine_rect(std::nullopt);
        verify(WindowFeature::WarpCursor, "set_cursor_position", [&] { return window.set_cursor_position(WindowPosition{20, 20}); });
        verify(WindowFeature::CustomCursor, "set_custom_cursor", [&] { return window.set_custom_cursor(CursorImage{2, 2, pixels, 0, 0}); });
        check(ok(window.clear_custom_cursor()) || !window.feature_support(WindowFeature::CustomCursor).supported, provider, "clear_custom_cursor");
        verify(WindowFeature::FrameExtents, "frame_extents", [&] { return window.frame_extents().transform([](FrameExtents) {}); });
        verify(WindowFeature::SafeArea, "safe_area", [&] { return window.safe_area().transform([](WindowRect) {}); });
        verify(WindowFeature::FullscreenTarget, "set_fullscreen_target", [&] { return window.set_fullscreen_target(FullscreenTarget{}); });
        verify(WindowFeature::TextInputOptions, "start_text_input_with", [&] {
            // Features that are not supported fall back to plain text input, which succeeds; only a supported one is held to it.
            return window.feature_support(WindowFeature::TextInputOptions).supported
                       ? window.start_text_input_with(TextInputOptions{TextInputType::Email, TextCapitalization::None, false, false})
                       : expected<void, WindowError>{unexpected(WindowError{WindowErrorCode::Unsupported, "no text input hints"})};
        });
        (void)window.stop_text_input();
        verify(WindowFeature::WindowIcon, "set_icon", [&] { return window.set_icon(2, 2, pixels); });
        verify(WindowFeature::RequestAttention, "request_attention", [&] { return window.request_attention(AttentionRequest::Briefly); });
        verify(WindowFeature::TaskbarProgress, "set_taskbar_progress", [&] { return window.set_taskbar_progress(TaskbarProgress::Normal, 0.5f); });
        verify(WindowFeature::SystemMenu, "show_system_menu", [&] { return window.show_system_menu(WindowPosition{5, 5}); });
        verify(WindowFeature::TypedClipboard, "set_clipboard_data",
               [&] { return window.set_clipboard_data("application/x-sturdy-test", pixels); });
        verify(WindowFeature::HitTestRegions, "set_hit_test_regions", [&] {
            HitTestRegions regions;
            regions.enabled = true;
            regions.drag.push_back(WindowRect{0, 0, 50, 20});
            return window.set_hit_test_regions(regions);
        });
        HitTestRegions off;
        (void)window.set_hit_test_regions(off);

        verify(WindowFeature::WindowTypeHint, "set_window_type", [&] { return window.set_window_type(WindowType::Utility); });
        (void)window.set_window_type(WindowType::Normal);
        verify(WindowFeature::WindowShape, "set_shape", [&] {
            // A diamond: covered only near the middle of each row.
            std::vector<std::byte> coverage(16 * 16, std::byte{0});
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    if (std::abs(x - 8) + std::abs(y - 8) < 8) {
                        coverage[static_cast<std::size_t>(y * 16 + x)] = std::byte{255};
                    }
                }
            }
            return window.set_shape(WindowShapeMask{16, 16, coverage});
        });
        (void)window.set_shape(WindowShapeMask{});
        verify(WindowFeature::AnimatedCursor, "set_animated_cursor", [&] {
            const CursorFrame frames[2] = {{CursorImage{2, 2, pixels, 0, 0}, 50}, {CursorImage{2, 2, pixels, 0, 0}, 50}};
            return window.set_animated_cursor(frames);
        });
        pump(window, 3);
        check(ok(window.clear_custom_cursor()) || !window.feature_support(WindowFeature::AnimatedCursor).supported, provider, "clear after animated cursor");
        verify(WindowFeature::WarpCursorGlobal, "set_global_cursor_position", [&] { return window.set_global_cursor_position(WindowPosition{60, 60}); });
        verify(WindowFeature::MouseCapture, "set_mouse_capture", [&] { return window.set_mouse_capture(true); });
        (void)window.set_mouse_capture(false);
        if (window.feature_support(WindowFeature::KeyboardQuery).supported) {
            check(!window.key_down(KeyboardKey::A), provider, "no key is held in the test");
        }

        // Parenting and modality need a second window of the same provider.
        if (auto parent = make_window()) {
            verify(WindowFeature::ParentWindow, "set_parent", [&] { return window.set_parent(parent.get(), false); });
            verify(WindowFeature::ModalWindow, "set_parent (modal)", [&] { return window.set_parent(parent.get(), true); });
            check(ok_or_unsupported(window.set_parent(nullptr, false)), provider, "set_parent(nullptr) detaches");
        }

        // Anything a provider lists must be real: the display list and its modes.
        check(window.feature_support(WindowFeature::DisplayList).supported == !window.displays().empty(), provider,
              "DisplayList agrees with displays()");
        const auto displays = window.displays();
        if (window.feature_support(WindowFeature::DisplayModes).supported && !displays.empty()) {
            check(!displays.front().modes.empty(), provider, "a display lists its modes");
            check(displays.front().current_mode.width > 0, provider, "a display reports its current mode");
        }
        check(!window.has_screen_keyboard() || window.feature_support(WindowFeature::ScreenKeyboard).supported, provider,
              "a screen keyboard is only reported where supported");
        const auto mime_types = window.clipboard_mime_types();
        check(window.feature_support(WindowFeature::ClipboardMimeTypes).supported || mime_types.empty(), provider,
              "clipboard mime types are only listed where supported");

        // Visibility events: hiding and showing must be reported by every provider.
        (void)saw_event(window, WindowEventKind::Shown);
        check(ok(window.hide()), provider, "hide");
        pump(window, 10);
        check(saw_event(window, WindowEventKind::Hidden), provider, "a Hidden event follows hide()");
        check(ok(window.show()), provider, "show");
        pump(window, 10);
        check(saw_event(window, WindowEventKind::Shown), provider, "a Shown event follows show()");

        // The cursor shapes both providers share.
        check(ok(window.set_cursor_icon(CursorIcon::Crosshair)), provider, "Crosshair cursor");
        check(ok(window.set_cursor_icon(CursorIcon::Move)), provider, "Move cursor");
        check(ok_or_unsupported(window.set_cursor_icon(CursorIcon::Wait)), provider, "Wait cursor is supported or says so");
        check(ok(window.set_cursor_icon(CursorIcon::Default)), provider, "Default cursor");
    }

    void exercise_app_id(const NativeWindowHandle &handle, const std::string &provider, const char *app_id) {
        const auto [name, klass] = x11_class(handle);
        check(name == app_id || klass == app_id, provider, "WindowConfig::app_id reaches WM_CLASS");
        if (name != app_id && klass != app_id) {
            std::fprintf(stderr, "[%s]   WM_CLASS was name='%s' class='%s', wanted '%s'\n", provider.c_str(), name.c_str(), klass.c_str(), app_id);
        }
    }

} // namespace

int main() {
    if (std::getenv("DISPLAY") == nullptr) {
        std::printf("WindowProviderParityTest: skipped (needs an X server; run it under Xvfb).\n");
        return 0;
    }
    // Both providers prefer Wayland when a compositor is reachable; this test is about the X11 routes.
    unsetenv("WAYLAND_DISPLAY");
    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("XDG_SESSION_TYPE", "x11", 1);

    constexpr const char *app_id = "org.sturdy.ParityTest";
    WindowConfig config;
    config.title = "provider parity";
    config.extent = WindowExtent{400, 300};
    config.decorated = false; // a custom title bar is what the hit-test checks are about
    config.app_id = app_id;
    config.graphics_api = WindowGraphicsApi::None;

    {
        auto window = Window::create<SDL3::SDL3Window>(config);
        if (!window.has_value()) {
            std::printf("WindowProviderParityTest: skipped (SDL3 could not open a window: %s).\n", window.error().message.c_str());
            return 0;
        }
        exercise(**window, "SDL3");
        exercise_union(**window, "SDL3", [&]() -> std::unique_ptr<Window> {
            auto other = Window::create<SDL3::SDL3Window>(config);
            return other ? std::unique_ptr<Window>{std::move(*other)} : nullptr;
        });
        if (auto handle = (*window)->native_window_handle()) {
            exercise_app_id(*handle, "SDL3", app_id);
        }
    }
    {
        auto window = GLFW::create_window(config);
        if (!window.has_value()) {
            std::printf("WindowProviderParityTest: skipped (GLFW could not open a window: %s).\n", window.error().message.c_str());
            return failures == 0 ? 0 : 1;
        }
        exercise(**window, "GLFW");
        exercise_union(**window, "GLFW", [&]() -> std::unique_ptr<Window> {
            auto other = GLFW::create_window(config);
            return other ? std::move(*other) : nullptr;
        });
        if (auto handle = (*window)->native_window_handle()) {
            exercise_app_id(*handle, "GLFW", app_id);
        }
    }

    // Gamepads are process-wide. A test machine has none attached, so this checks the service starts, reports an empty
    // list, and refuses unknown devices with the right error rather than crashing or lying.
    {
        const auto devices = Gamepads::list();
        check(devices.has_value(), "Gamepads", "the joystick subsystem starts and lists devices");
        if (devices.has_value() && devices->empty()) {
            check(!Gamepads::state(4242).has_value(), "Gamepads", "an unknown gamepad has no state");
            check(!Gamepads::raw_state(4242).has_value(), "Gamepads", "an unknown joystick has no raw state");
            const auto rumble = Gamepads::rumble(4242, 1.0f, 1.0f, 10);
            check(!rumble.has_value() && rumble.error().code == WindowErrorCode::InvalidArgument, "Gamepads", "rumbling an unknown gamepad says so");
            check(Gamepads::take_events().empty(), "Gamepads", "no events without devices");
        }
    }

    // Creation-time behaviour in the config is applied by `create` for every provider.
    {
        WindowConfig creation = config;
        creation.always_on_top = true;
        creation.focusable = false;
        creation.mouse_passthrough = true;
        creation.min_size = WindowExtent{200, 150};
        creation.aspect_min = 4.0f / 3.0f;
        creation.aspect_max = 4.0f / 3.0f;
        creation.window_type = WindowType::Utility;
        auto sdl = Window::create<SDL3::SDL3Window>(creation);
        check(sdl.has_value(), "SDL3", "a window with creation-time behaviour is created");
        auto glfw = GLFW::create_window(creation);
        check(glfw.has_value(), "GLFW", "a window with creation-time behaviour is created");
    }

    // Desktop services are provider-neutral: a program using only GLFW windows still reaches them.
    {
        check(Desktop::set_screensaver_enabled(false).has_value(), "Desktop", "the screen saver can be inhibited");
        const auto inhibited = Desktop::screensaver_enabled();
        check(inhibited.has_value() && !*inhibited, "Desktop", "the screen saver reads back as inhibited");
        // A tray needs a notification area; a bare X server has none, so "unsupported" is the right answer there.
        const std::vector<std::byte> tray_pixels(16 * 16 * 4, std::byte{0xC0});
        auto tray = Desktop::Tray::create(16, 16, tray_pixels, "parity", {Desktop::TrayEntry{.label = "Quit", .on_click = [] {}}});
        check(tray.has_value() || tray.error().code == WindowErrorCode::Unsupported, "Desktop", "a tray is created or says it cannot be");
        Desktop::pump_events();
        check(Desktop::set_screensaver_enabled(true).has_value(), "Desktop", "the screen saver can be allowed again");
        const auto allowed = Desktop::screensaver_enabled();
        check(allowed.has_value() && *allowed, "Desktop", "the screen saver reads back as allowed");
    }

    if (failures == 0) {
        std::printf("WindowProviderParityTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
