#include <WindowManager/Window.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::WindowManager::Detail {

/// Allocates window ID.
///
/// @return Returns the current allocate window ID value.
/// @note This function does not throw exceptions.
WindowId allocate_window_id() noexcept {
            ZoneScopedN("Windowing::Detail::allocate_window_id");
            static std::atomic<usize> next_id{0};
            return static_cast<WindowId>(next_id.fetch_add(1, std::memory_order_relaxed));
        }

} // namespace SFT::WindowManager::Detail

namespace SFT::WindowManager {

/// Performs the window operation for `Windowing` using the supplied arguments.
///
/// @note This function does not throw exceptions.
Window::Window(ConstructorKey) noexcept
            : id_(Detail::allocate_window_id()) {}

namespace {
    [[nodiscard]] unexpected<WindowError> chrome_unsupported() noexcept {
        return unexpected(WindowError{WindowErrorCode::Unsupported, "This window provider does not implement window chrome control."});
    }
} // namespace

expected<void, WindowError> Window::set_hit_test_regions(HitTestRegions) noexcept { return chrome_unsupported(); }
expected<void, WindowError> Window::show_system_menu(WindowPosition) noexcept { return chrome_unsupported(); }
bool Window::maximized() const noexcept { return false; }
bool Window::minimized() const noexcept { return false; }
expected<void, WindowError> Window::set_icon(u32, u32, std::span<const std::byte>) noexcept { return chrome_unsupported(); }
expected<void, WindowError> Window::request_attention(AttentionRequest) noexcept { return chrome_unsupported(); }
expected<void, WindowError> Window::set_taskbar_progress(TaskbarProgress, f32) noexcept { return chrome_unsupported(); }
SystemTheme Window::system_theme() const noexcept { return SystemTheme::Unknown; }
std::vector<DisplayInfo> Window::displays() const noexcept { return {}; }

namespace {
    [[nodiscard]] unexpected<WindowError> feature_unsupported(const char *what) noexcept {
        return unexpected(WindowError{WindowErrorCode::Unsupported, what});
    }
} // namespace

FeatureSupport Window::feature_support(WindowFeature feature) const noexcept {
    // The features the base class builds on the native handle are as supported as that handle's effect route.
    const auto native_effect = [this](WindowEffectKind kind) {
        const WindowEffectSupport support = window_effect_support(kind);
        if (support.availability == EffectAvailability::Available) {
            return FeatureSupport::yes();
        }
        if (support.availability == EffectAvailability::Unknown) {
            return FeatureSupport::yes("the window system does not confirm whether it honours this");
        }
        return FeatureSupport::no(support.route_count > 0 && !support.routes[0].reason.empty() ? support.routes[0].reason
                                                                                              : string_view{"not available on this platform"});
    };
    switch (feature) {
        case WindowFeature::MousePassthrough: return native_effect(WindowEffectKind::MousePassthrough);
        case WindowFeature::Focusable: return native_effect(WindowEffectKind::Focusable);
        case WindowFeature::KeyboardGrab: return native_effect(WindowEffectKind::KeyboardGrab);
        case WindowFeature::WindowTypeHint:
        case WindowFeature::WindowShape: {
            const auto handle = native_window_handle();
            const NativeWindowSystem system = handle ? handle->system : NativeWindowSystem::Unknown;
            const bool shape = feature == WindowFeature::WindowShape;
            if (system == NativeWindowSystem::X11 || system == NativeWindowSystem::Win32) {
                return shape ? FeatureSupport::yes("hard-edged: the outline is a 1-bit region") : FeatureSupport::yes();
            }
            if (system == NativeWindowSystem::Cocoa && !shape) {
                return FeatureSupport::yes("approximated with the window level");
            }
            return FeatureSupport::no(system == NativeWindowSystem::Wayland ? "Wayland leaves window roles and outlines to the compositor"
                                                                           : "this window system has no such request");
        }
        case WindowFeature::SafeArea: return FeatureSupport::yes("the whole client area");
        case WindowFeature::ClipboardMimeTypes: return FeatureSupport::yes("text only");
        case WindowFeature::SystemTray: return FeatureSupport::yes("provided by Desktop::Tray, whatever the window provider");
        default: return FeatureSupport::no("this window provider does not implement it");
    }
}

namespace {
    /// Effect-backed toggles shared by every provider: refuse up front when the platform has no route for them.
    expected<void, WindowError> toggle_native_effect(Window &window, WindowFeature feature, WindowEffectKind kind, bool enabled,
                                                     const char *unsupported) noexcept {
        if (!window.feature_support(feature).supported) {
            return feature_unsupported(unsupported);
        }
        return window_result_from_effect_result(window.enable_window_effect(WindowEffect{kind, enabled, 0, LinuxBlurProtocol::Automatic}));
    }
} // namespace

expected<void, WindowError> Window::set_always_on_top(bool) noexcept { return feature_unsupported("This window provider cannot keep a window on top."); }
expected<void, WindowError> Window::set_aspect_ratio(f32, f32) noexcept { return feature_unsupported("This window provider cannot constrain the aspect ratio."); }
expected<void, WindowError> Window::set_mouse_passthrough(bool enabled) noexcept {
    return toggle_native_effect(*this, WindowFeature::MousePassthrough, WindowEffectKind::MousePassthrough, enabled,
                                "This window system cannot make a window click-through.");
}
expected<void, WindowError> Window::set_focusable(bool enabled) noexcept {
    return toggle_native_effect(*this, WindowFeature::Focusable, WindowEffectKind::Focusable, enabled,
                                "This window system cannot make a window refuse focus.");
}
expected<void, WindowError> Window::set_auto_minimize_on_focus_loss(bool) noexcept {
    return feature_unsupported("This window provider cannot control minimising on focus loss.");
}
expected<void, WindowError> Window::set_parent(Window *parent, bool modal) noexcept {
    const auto child_handle = native_window_handle();
    if (!child_handle) {
        return unexpected(child_handle.error());
    }
    std::optional<NativeWindowHandle> parent_handle;
    if (parent != nullptr) {
        const auto handle = parent->native_window_handle();
        if (!handle) {
            return unexpected(handle.error());
        }
        parent_handle = *handle;
    }
    return window_result_from_effect_result(set_native_window_parent(*child_handle, parent_handle, modal));
}
expected<void, WindowError> Window::set_keyboard_grab(bool enabled) noexcept {
    return toggle_native_effect(*this, WindowFeature::KeyboardGrab, WindowEffectKind::KeyboardGrab, enabled,
                                "This window system cannot grab the keyboard.");
}
expected<void, WindowError> Window::set_mouse_confine_rect(std::optional<WindowRect>) noexcept {
    return feature_unsupported("This window provider cannot confine the cursor to a rectangle.");
}
expected<void, WindowError> Window::set_cursor_position(WindowPosition) noexcept {
    return feature_unsupported("This window provider cannot move the cursor.");
}
expected<void, WindowError> Window::set_custom_cursor(const CursorImage &) noexcept {
    return feature_unsupported("This window provider cannot show a custom cursor image.");
}
expected<void, WindowError> Window::clear_custom_cursor() noexcept {
    return feature_unsupported("This window provider cannot show a custom cursor image.");
}
expected<FrameExtents, WindowError> Window::frame_extents() const noexcept {
    return feature_unsupported("This window provider cannot report the decoration size.");
}
expected<WindowRect, WindowError> Window::safe_area() const noexcept {
    const auto extent = size();
    if (!extent) {
        return unexpected(extent.error());
    }
    return WindowRect{0, 0, static_cast<i32>(extent->x), static_cast<i32>(extent->y)};
}
expected<void, WindowError> Window::start_text_input_with(const TextInputOptions &) noexcept { return start_text_input(); }
bool Window::has_screen_keyboard() const noexcept { return false; }
bool Window::screen_keyboard_shown() const noexcept { return false; }
std::vector<std::string> Window::clipboard_mime_types() const noexcept {
    if (clipboard_text().empty()) {
        return {};
    }
    return {"text/plain;charset=utf-8"};
}
expected<void, WindowError> Window::set_fullscreen_target(FullscreenTarget) noexcept {
    return feature_unsupported("This window provider cannot choose the fullscreen display or mode.");
}

expected<void, WindowError> Window::set_window_type(WindowType type) noexcept {
    if (!feature_support(WindowFeature::WindowTypeHint).supported) {
        return feature_unsupported("This window system has no window type request.");
    }
    const auto handle = native_window_handle();
    if (!handle) {
        return unexpected(handle.error());
    }
    return window_result_from_effect_result(set_native_window_type(*handle, type));
}

expected<void, WindowError> Window::set_shape(const WindowShapeMask &mask) noexcept {
    if (!feature_support(WindowFeature::WindowShape).supported) {
        return feature_unsupported("This window system cannot give a window a non-rectangular outline.");
    }
    const auto handle = native_window_handle();
    const auto extent = framebuffer_size();
    if (!handle) {
        return unexpected(handle.error());
    }
    if (!extent) {
        return unexpected(extent.error());
    }
    if (!mask.coverage.empty() && (mask.width == 0 || mask.height == 0 || mask.coverage.size() < static_cast<usize>(mask.width) * mask.height)) {
        return unexpected(WindowError{WindowErrorCode::InvalidArgument, "A shape mask needs width * height coverage bytes."});
    }
    return window_result_from_effect_result(set_native_window_shape(*handle, mask, *extent));
}

expected<void, WindowError> Window::set_animated_cursor(std::span<const CursorFrame>) noexcept {
    return feature_unsupported("This window provider cannot show an animated cursor.");
}
expected<void, WindowError> Window::set_global_cursor_position(WindowPosition) noexcept {
    return feature_unsupported("This window provider cannot move the cursor on the desktop.");
}
expected<void, WindowError> Window::set_mouse_capture(bool) noexcept {
    return feature_unsupported("This window provider cannot capture the mouse.");
}
std::string Window::key_name(KeyboardKey) const noexcept { return {}; }
bool Window::key_down(KeyboardKey) const noexcept { return false; }
expected<std::vector<std::byte>, WindowError> Window::icc_profile() const noexcept {
    return feature_unsupported("This window provider cannot read the display's colour profile.");
}
expected<void, WindowError> Window::set_display_gamma(std::optional<u32>, f32) noexcept {
    return feature_unsupported("This window provider cannot change a display's gamma.");
}
std::optional<u32> Window::current_display_id() const noexcept { return std::nullopt; }

void Window::apply_creation_config(const WindowConfig &config) noexcept {
    // Each field goes through its setter; one the provider or platform cannot honour is skipped, not fatal.
    const auto note = [](const char *what, const expected<void, WindowError> &result) {
        if (!result.has_value()) {
            Detail::window_warn("Window creation option '{}' was not applied: {}", what, result.error().message);
        }
    };
    if (config.window_type != WindowType::Normal) {
        note("window_type", set_window_type(config.window_type));
    }
    if (config.always_on_top) {
        note("always_on_top", set_always_on_top(true));
    }
    if (config.min_size.x != 0 || config.min_size.y != 0) {
        note("min_size", set_minimum_size(config.min_size));
    }
    if (config.max_size.x != 0 || config.max_size.y != 0) {
        note("max_size", set_maximum_size(config.max_size));
    }
    if (config.aspect_min != 0.0f || config.aspect_max != 0.0f) {
        note("aspect", set_aspect_ratio(config.aspect_min, config.aspect_max));
    }
    if (!config.focusable) {
        note("focusable", set_focusable(false));
    }
    if (config.mouse_passthrough) {
        note("mouse_passthrough", set_mouse_passthrough(true));
    }
    if (config.auto_minimize_on_focus_loss.has_value()) {
        note("auto_minimize_on_focus_loss", set_auto_minimize_on_focus_loss(*config.auto_minimize_on_focus_loss));
    }
    if (config.parent != nullptr) {
        note("parent", set_parent(config.parent, config.modal));
    }
    if (config.fullscreen_target.display_id.has_value() || config.fullscreen_target.mode.has_value()) {
        note("fullscreen_target", set_fullscreen_target(config.fullscreen_target));
    }
    if (config.maximized) {
        note("maximized", maximize());
    }
    if (config.minimized) {
        note("minimized", minimize());
    }
}

WindowEffectSupport Window::window_effect_support(WindowEffectKind kind) const noexcept {
    const auto handle = native_window_handle();
    if (!handle) {
        WindowEffectSupport support;
        support.add("none", EffectAvailability::Unknown, "the window has no native handle yet");
        return support;
    }
    return query_native_window_effect_support(*handle, kind);
}

/// Returns the current or globally available ID value.
///
/// @return Returns the current ID value.
/// @note This function does not throw exceptions.
[[nodiscard]] WindowId Window::id() const noexcept { return id_; }

/// Acquires the associated synchronization primitive before protected access.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
expected<void, WindowError> Window::lock_mouse_to_window() noexcept {
            ZoneScopedN("Window::lock_mouse_to_window");
            return set_mouse_locked(true);
        }

/// Releases the associated synchronization primitive.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
expected<void, WindowError> Window::unlock_mouse() noexcept {
            ZoneScopedN("Window::unlock_mouse");
            return set_mouse_locked(false);
        }

/// Handles the set live resize callback callback and updates the associated platform state.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void Window::set_live_resize_callback(std::function<void(WindowExtent)>             ) noexcept {}

bool Window::wait_for_native_events(std::chrono::microseconds) noexcept { return false; }

void (*Window::native_event_waker() const noexcept)() noexcept { return nullptr; }

bool Window::clipboard_has_mime(std::string_view) const noexcept { return false; }

std::vector<std::byte> Window::clipboard_data(std::string_view) const noexcept { return {}; }

expected<void, WindowError> Window::set_clipboard_data(std::string_view, std::span<const std::byte>) noexcept {
    return unexpected(WindowError{WindowErrorCode::Unsupported, "This window provider does not implement typed clipboard data."});
}

} // namespace SFT::WindowManager

namespace SFT::WindowManager {

    /// Returns the current or globally available HDR properties value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<WindowHdrProperties> Window::hdr_properties() const noexcept {
        return std::nullopt;
    }

    /// Returns the current or globally available global cursor position value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    expected<WindowPosition, WindowError> Window::global_cursor_position() const noexcept {
        return unexpected(WindowError{
            WindowErrorCode::Unsupported,
            "Global cursor position is unavailable for this window provider.",
        });
    }

    /// Returns the content scale; providers without a platform query report 1.0.
    ///
    /// @return Always 1.0 for the base implementation.
    /// @note This function does not throw exceptions.
    f32 Window::content_scale() const noexcept { return 1.0f; }

    /// Starts text input using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> Window::start_text_input() noexcept { return {}; }

    /// Stops text input using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> Window::stop_text_input() noexcept { return {}; }

    /// Sets the text input area for this `Windowing`.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> Window::set_text_input_area(TextInputArea         ) noexcept { return {}; }

} // namespace SFT::WindowManager

