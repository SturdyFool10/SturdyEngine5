#include <WindowManager/Window.hpp>

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

