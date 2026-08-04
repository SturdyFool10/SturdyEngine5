#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <SDL3/SDL.h>

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
                case 6: return MouseButton::Extra3;
                case 7: return MouseButton::Extra4;
                case 8: return MouseButton::Extra5;
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

        void set_repaint_callback(std::function<void()> callback) noexcept override;

        [[nodiscard]] std::string clipboard_text() const noexcept override;
        expected<void, WindowError> set_clipboard_text(std::string_view text) noexcept override;

      private:
        friend class ::SFT::Platform::Windowing::Window;

        SDL3Window(ConstructorKey key, SDL_Window *window) noexcept;

        SDL_Window *window_ = nullptr;
        deque<WindowEvent> events_;
        optional<WindowResize> pending_resize_;
        WindowExtent last_size_ = {};
        WindowExtent last_framebuffer_size_ = {};
        atomic_bool close_requested_ = false;
        bool mouse_locked_ = false;
        std::function<void()> repaint_callback_;
        SDL_Cursor *current_cursor_ = nullptr;
        optional<CursorIcon> current_cursor_icon_;

        static bool SDLCALL sdl_repaint_watch(void *userdata, SDL_Event *event) noexcept;
    };

} // namespace SFT::Platform::Windowing::SDL3
