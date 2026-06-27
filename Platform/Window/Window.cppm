module;

#include <concepts>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <utility>

export module Sturdy.Platform:Window;

import Sturdy.Foundation;

export import :WindowError;
export import :WindowGeometry;
export import :WindowEvent;
export import :WindowEffect;
export import :WindowConfig;

using std::bad_alloc;
using std::derived_from;
using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;

export namespace SFT::Platform::Windowing {

    enum class WindowBackendKind {
        SDL3,
        GLFW,
    };

    enum class WindowingSystem {
        Unknown,
        SDL3,
        GLFW,
    };

    class Window {
      protected:
        struct ConstructorKey {
          private:
            friend class Window;
            constexpr ConstructorKey() = default;
        };

        explicit constexpr Window(ConstructorKey) noexcept {}

      public:
        virtual ~Window() noexcept = default;

        Window(const Window &) = delete;
        Window &operator=(const Window &) = delete;
        Window(Window &&) = delete;
        Window &operator=(Window &&) = delete;

        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> create(Args &&...args) noexcept {
            try {
                return Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            } catch (const bad_alloc &) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory while creating window."});
            } catch (...) {
                return unexpected(WindowError{WindowErrorCode::CreationFailed, "Unexpected exception while creating window."});
            }
        }

        [[nodiscard]] virtual WindowBackendKind backend_kind() const noexcept = 0;
        [[nodiscard]] virtual WindowingSystem type() const noexcept = 0;
        [[nodiscard]] virtual expected<void *, WindowError> native_backend_handle() const noexcept = 0;
        [[nodiscard]] virtual expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept = 0;

        virtual expected<void, WindowError> pump_events() noexcept = 0;
        [[nodiscard]] virtual optional<WindowEvent> poll_event() noexcept = 0;
        [[nodiscard]] virtual bool close_requested() const noexcept = 0;
        virtual void request_close() noexcept = 0;
        [[nodiscard]] virtual bool resized() const noexcept = 0;
        [[nodiscard]] virtual optional<WindowResize> consume_resize() noexcept = 0;

        virtual expected<void, WindowError> show() noexcept = 0;
        virtual expected<void, WindowError> hide() noexcept = 0;
        virtual expected<void, WindowError> focus() noexcept = 0;
        virtual expected<void, WindowError> raise() noexcept = 0;
        virtual expected<void, WindowError> maximize() noexcept = 0;
        virtual expected<void, WindowError> minimize() noexcept = 0;
        virtual expected<void, WindowError> restore() noexcept = 0;

        virtual expected<void, WindowError> set_title(const char *title) noexcept = 0;
        [[nodiscard]] virtual expected<WindowPosition, WindowError> position() const noexcept = 0;
        virtual expected<void, WindowError> set_position(WindowPosition position) noexcept = 0;
        [[nodiscard]] virtual expected<WindowExtent, WindowError> size() const noexcept = 0;
        virtual expected<void, WindowError> set_size(WindowExtent extent) noexcept = 0;
        [[nodiscard]] virtual expected<WindowExtent, WindowError> framebuffer_size() const noexcept = 0;
        virtual expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept = 0;
        virtual expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept = 0;

        virtual expected<void, WindowError> set_resizable(bool enabled) noexcept = 0;
        virtual expected<void, WindowError> set_decorated(bool enabled) noexcept = 0;
        virtual expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept = 0;
        virtual expected<void, WindowError> set_opacity(f32 opacity) noexcept = 0;
        [[nodiscard]] virtual expected<f32, WindowError> opacity() const noexcept = 0;

        virtual expected<void, WindowError> set_cursor_visible(bool visible) noexcept = 0;
        virtual expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept = 0;
        virtual expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept = 0;
        virtual expected<void, WindowError> set_mouse_locked(bool locked) noexcept = 0;
        [[nodiscard]] virtual bool mouse_locked() const noexcept = 0;

        expected<void, WindowError> lock_mouse_to_window() noexcept {
            return set_mouse_locked(true);
        }

        expected<void, WindowError> unlock_mouse() noexcept {
            return set_mouse_locked(false);
        }

        [[nodiscard]] virtual WindowEffectResult enable_window_effect(WindowEffect effect) noexcept = 0;
        virtual expected<void, WindowError> set_effect(WindowEffect effect) noexcept = 0;
        virtual expected<void, WindowError> set_blur_enabled(bool enabled) noexcept = 0;
    };

} // namespace SFT::Platform::Windowing
