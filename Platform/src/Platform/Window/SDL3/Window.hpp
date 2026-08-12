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

        [[nodiscard]] constexpr MouseButton normalize_mouse_button(u8 button) noexcept {
            switch (button) {
                case SDL_BUTTON_LEFT: return MouseButton::Left;
                case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
                case SDL_BUTTON_RIGHT: return MouseButton::Right;
                case SDL_BUTTON_X1: return MouseButton::Extra1;
                case SDL_BUTTON_X2: return MouseButton::Extra2;
                // SDL reports anything past X1/X2 as a plain raw index with no further naming (a
                // high-button-count gaming mouse) — MouseButton::Extra3.. is this engine's own
                // numbering for that range, not an SDL one.
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
        ~SDL3Window() noexcept override;

        [[nodiscard]] static expected<unique_ptr<SDL3Window>, WindowError> construct(ConstructorKey key, const WindowConfig &config) noexcept;

        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override;
        [[nodiscard]] WindowingSystem type() const noexcept override;
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override;
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override;
        [[nodiscard]] optional<WindowHdrProperties> hdr_properties() const noexcept override;

        expected<void, WindowError> pump_events() noexcept override;
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override;
        [[nodiscard]] bool close_requested() const noexcept override;
        void request_close() noexcept override;
        [[nodiscard]] bool resized() const noexcept override;
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override;

        expected<void, WindowError> show() noexcept override;
        expected<void, WindowError> hide() noexcept override;
        expected<void, WindowError> focus() noexcept override;
        expected<void, WindowError> raise() noexcept override;
        expected<void, WindowError> maximize() noexcept override;
        expected<void, WindowError> minimize() noexcept override;
        expected<void, WindowError> restore() noexcept override;

        expected<void, WindowError> set_title(const char *title) noexcept override;
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override;
        expected<void, WindowError> set_position(WindowPosition position) noexcept override;
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override;
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override;
        expected<void, WindowError> set_size(WindowExtent extent) noexcept override;
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override;
        expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept override;
        expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept override;

        expected<void, WindowError> set_resizable(bool enabled) noexcept override;
        expected<void, WindowError> set_decorated(bool enabled) noexcept override;
        expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept override;
        expected<void, WindowError> set_opacity(f32 opacity) noexcept override;
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override;

        expected<void, WindowError> set_cursor_visible(bool visible) noexcept override;
        expected<void, WindowError> set_cursor_icon(CursorIcon icon) noexcept override;
        expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept override;
        expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept override;
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override;
        [[nodiscard]] bool mouse_locked() const noexcept override;

        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect effect) noexcept override;
        expected<void, WindowError> set_effect(WindowEffect effect) noexcept override;
        expected<void, WindowError> set_blur_enabled(bool enabled) noexcept override;
        expected<void, WindowError> set_transparent(bool enabled) noexcept override;

        [[nodiscard]] expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept override;
        expected<void, WindowError> create_vulkan_surface(
            void *instance,
            const void *allocation_callbacks,
            void *surface_out) const noexcept override;

        void set_live_resize_callback(std::function<void(WindowExtent)> callback) noexcept override;

        [[nodiscard]] std::string clipboard_text() const noexcept override;
        expected<void, WindowError> set_clipboard_text(std::string_view text) noexcept override;

        expected<void, WindowError> start_text_input() noexcept override;
        expected<void, WindowError> stop_text_input() noexcept override;
        expected<void, WindowError> set_text_input_area(TextInputArea area) noexcept override;

      private:
        friend class ::SFT::Platform::Windowing::Window;

        SDL3Window(ConstructorKey key, SDL_Window *window) noexcept;

        // Shared body of native_window_handle(), callable from a caller that already holds
        // sdl_window_mutex() (enable_window_effect()) without re-locking it — sdl_window_mutex() is
        // a non-recursive Async::Mutex, so a second lock() call from the same thread would deadlock.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle_locked() const noexcept;

        SDL_Window *window_ = nullptr;
        deque<WindowEvent> events_;
        optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};
        // Updated by the Windows modal-loop event watch under sdl_window_mutex(). Kept separate
        // from last_framebuffer_size_, which is committed only when the ordinary event pump drains.
        WindowExtent last_live_resize_extent_ = {};
        atomic_bool close_requested_ = false;
        bool mouse_locked_ = false;
        std::function<void(WindowExtent)> live_resize_callback_;
        SDL_Cursor *current_cursor_ = nullptr;
        optional<CursorIcon> current_cursor_icon_;
        // Wayland may destroy/recreate wl_surface across hide/show. Retain the requested blur state
        // and last complete native handle so protocol objects can be released before hide, reapplied
        // after show, and display-scoped state can still be torn down while the window is hidden.
        optional<WindowEffect> active_blur_effect_;
        optional<NativeWindowHandle> native_effect_handle_;
        // Cached compositor-preferred reference white. Invalidated when SDL reports a display/HDR
        // transition or recreates the Wayland surface; querying it performs private-queue roundtrips.
        mutable bool wayland_reference_white_queried_ = false;
        mutable optional<f32> wayland_reference_white_nits_;
        mutable u64 wayland_reference_white_query_time_ns_ = 0;

        static bool SDLCALL sdl_live_resize_watch(void *userdata, SDL_Event *event) noexcept;
#if defined(_WIN32)
        static bool SDLCALL sdl_windows_message_hook(void *userdata, MSG *message) noexcept;
#endif
    };

} // namespace SFT::Platform::Windowing::SDL3
