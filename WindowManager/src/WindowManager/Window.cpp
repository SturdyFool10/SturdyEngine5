#include <WindowManager/Window.hpp>
#include <WindowManager/WindowEffects.hpp>

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

