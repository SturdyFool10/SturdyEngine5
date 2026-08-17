#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <SDL3/SDL.h>
#if defined(_WIN32)
#include <SDL3/SDL_system.h>
#endif

#include <atomic>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include <Platform/Platform.hpp>

using std::atomic_bool;
using std::deque;
using std::expected;
using std::optional;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing::SDL3 {

    namespace Detail {

        /// Normalizes mouse button using the supplied arguments and current state.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr MouseButton normalize_mouse_button(u8 button) noexcept {
            switch (button) {
                case SDL_BUTTON_LEFT: return MouseButton::Left;
                case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
                case SDL_BUTTON_RIGHT: return MouseButton::Right;
                case SDL_BUTTON_X1: return MouseButton::Extra1;
                case SDL_BUTTON_X2: return MouseButton::Extra2;


                case 6: return MouseButton::Extra3;
                case 7: return MouseButton::Extra4;
                case 8: return MouseButton::Extra5;
                case 9: return MouseButton::Extra6;
                case 10: return MouseButton::Extra7;
                case 11: return MouseButton::Extra8;
                case 12: return MouseButton::Extra9;
                case 13: return MouseButton::Extra10;
                case 14: return MouseButton::Extra11;
                case 15: return MouseButton::Extra12;
                default: return MouseButton::Unknown;
            }
        }

    } // namespace Detail

    class SDL3Window final : public Window {
      public:
        /// Destroys the `SDL3Window` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~SDL3Window() noexcept override;

        /// Constructs the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param key Key used to identify the requested entry.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static expected<unique_ptr<SDL3Window>, WindowError> construct(ConstructorKey key, const WindowConfig &config) noexcept;

        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
        /// Returns the runtime or backend type represented by `SDL3Window`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowingSystem type() const noexcept override;
        /// Returns the native backend handle associated with this `SDL3Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override;
        /// Returns the native window handle associated with this `SDL3Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override;
        /// Returns the current or globally available HDR properties value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowHdrProperties> hdr_properties() const noexcept override;

        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> pump_events() noexcept override;
        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override;
        /// Closes requested using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool close_requested() const noexcept override;
        /// Requests close using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void request_close() noexcept override;
        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool resized() const noexcept override;
        /// Returns the current or globally available consume resize value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override;

        /// Returns the current or globally available show value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> show() noexcept override;
        /// Returns the current or globally available hide value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> hide() noexcept override;
        /// Returns the current or globally available focus value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> focus() noexcept override;
        /// Returns the current or globally available raise value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> raise() noexcept override;
        /// Returns the current or globally available maximize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> maximize() noexcept override;
        /// Returns the current or globally available minimize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> minimize() noexcept override;
        /// Returns the current or globally available restore value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> restore() noexcept override;

        /// Sets the title for this `SDL3Window`.
        ///
        /// @param title `title` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_title(const char *title) noexcept override;
        /// Returns the current or globally available position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override;
        /// Sets the position for this `SDL3Window`.
        ///
        /// @param position `position` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_position(WindowPosition position) noexcept override;
        /// Returns the current or globally available global cursor position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override;
        /// Returns the size for this `SDL3Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override;
        /// Sets the size for this `SDL3Window`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_size(WindowExtent extent) noexcept override;
        /// Returns the framebuffer size for this `SDL3Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override;
        /// Sets the minimum size for this `SDL3Window`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept override;
        /// Sets the maximum size for this `SDL3Window`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept override;

        /// Sets the resizable for this `SDL3Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_resizable(bool enabled) noexcept override;
        /// Sets the decorated for this `SDL3Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_decorated(bool enabled) noexcept override;
        /// Sets the fullscreen for this `SDL3Window`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept override;
        /// Returns the current or globally available fullscreen mode value.
        ///
        /// @return Returns the current fullscreen mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowMode fullscreen_mode() const noexcept override;
        /// Sets the opacity for this `SDL3Window`.
        ///
        /// @param opacity `opacity` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_opacity(f32 opacity) noexcept override;
        /// Returns the current or globally available opacity value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override;

        /// Sets the cursor visible for this `SDL3Window`.
        ///
        /// @param visible `visible` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_visible(bool visible) noexcept override;
        /// Sets the cursor icon for this `SDL3Window`.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_icon(CursorIcon icon) noexcept override;
        /// Sets the cursor grabbed for this `SDL3Window`.
        ///
        /// @param grabbed `grabbed` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept override;
        /// Sets the relative mouse mode for this `SDL3Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept override;
        /// Sets the mouse locked for this `SDL3Window`.
        ///
        /// @param locked `locked` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override;
        /// Returns the current or globally available mouse locked value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_locked() const noexcept override;

        /// Enables window effect using the supplied arguments and current state.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
        /// Sets the effect for this `SDL3Window`.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_effect(WindowEffect effect) noexcept override;
        /// Sets the blur enabled for this `SDL3Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_blur_enabled(bool enabled) noexcept override;
        /// Sets the transparent for this `SDL3Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_transparent(bool enabled) noexcept override;

        /// Returns the current or globally available required vulkan instance extensions value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept override;
        /// Creates a vulkan surface from the supplied parameters.
        ///
        /// @param instance Instance used or affected by the operation.
        /// @param allocation_callbacks Callable invoked by the operation.
        /// @param surface_out Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> create_vulkan_surface(
            void *instance,
            const void *allocation_callbacks,
            void *surface_out) const noexcept override;

        /// Sets the live resize callback for this `SDL3Window`.
        ///
        /// @param callback Callable invoked by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_live_resize_callback(std::function<void(WindowExtent)> callback) noexcept override;

        /// Returns the current or globally available clipboard text value.
        ///
        /// @return Returns the current clipboard text value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string clipboard_text() const noexcept override;
        /// Sets the clipboard text for this `SDL3Window`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_clipboard_text(std::string_view text) noexcept override;

        /// Starts text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> start_text_input() noexcept override;
        /// Stops text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> stop_text_input() noexcept override;
        /// Sets the text input area for this `SDL3Window`.
        ///
        /// @param area `area` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_text_input_area(TextInputArea area) noexcept override;

      private:
        friend class ::SFT::Platform::Windowing::Window;

        /// Constructs a `SDL3Window` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        /// @param window Window used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        SDL3Window(ConstructorKey key, SDL_Window *window) noexcept;


        /// Returns the current or globally available native window handle locked value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_locked() const noexcept;

        SDL_Window *window_ = nullptr;
        deque<WindowEvent> events_;
        optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};


        WindowExtent last_live_resize_extent_ = {};
#if defined(_WIN32)


        bool use_windows_sizing_hook_ = false;
#endif
        atomic_bool close_requested_ = false;
        bool mouse_locked_ = false;


        WindowMode fullscreen_mode_ = WindowMode::Windowed;
        std::function<void(WindowExtent)> live_resize_callback_;
        SDL_Cursor *current_cursor_ = nullptr;
        optional<CursorIcon> current_cursor_icon_;


        optional<WindowEffect> active_blur_effect_;
        optional<NativeWindowHandle> native_effect_handle_;


        mutable bool wayland_reference_white_queried_ = false;
        mutable optional<f32> wayland_reference_white_nits_;
        mutable u64 wayland_reference_white_query_time_ns_ = 0;

        /// Performs the SDL live resize watch operation for `SDL3Window` using the supplied arguments.
        ///
        /// @param userdata `userdata` value used by the operation.
        /// @param event Event used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        static bool SDLCALL sdl_live_resize_watch(void *userdata, SDL_Event *event) noexcept;
#if defined(_WIN32)
        /// Performs the SDL windows message hook operation for `SDL3Window` using the supplied arguments.
        ///
        /// @param userdata `userdata` value used by the operation.
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        static bool SDLCALL sdl_windows_message_hook(void *userdata, MSG *message) noexcept;
#endif
    };

} // namespace SFT::Platform::Windowing::SDL3
