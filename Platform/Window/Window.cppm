module;

#include <atomic>
#include <concepts>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

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
using std::vector;

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

    // Stable per-window identity, assigned once at construction and never reused — engine
    // backends key all per-window GPU resources (surface, swapchain, sync objects, ...) by
    // this ID rather than by pointer or by a recyclable slot index.
    enum class WindowId : usize {};

    inline constexpr WindowId invalid_window_id = static_cast<WindowId>(static_cast<usize>(~usize{0}));

    namespace Detail {

        // Monotonically increasing across the lifetime of the process; ids are never reused
        // even after a window is destroyed. The function-local static gives one shared counter
        // across every translation unit that imports this partition.
        [[nodiscard]] inline WindowId allocate_window_id() noexcept {
            static std::atomic<usize> next_id{0};
            return static_cast<WindowId>(next_id.fetch_add(1, std::memory_order_relaxed));
        }

    } // namespace Detail

    class Window {
      protected:
        struct ConstructorKey {
          private:
            friend class Window;
            constexpr ConstructorKey() = default;
        };

        explicit Window(ConstructorKey) noexcept
            : id_(Detail::allocate_window_id()) {}

      public:
        virtual ~Window() noexcept = default;

        Window(const Window &) = delete;
        Window &operator=(const Window &) = delete;
        Window(Window &&) = delete;
        Window &operator=(Window &&) = delete;

        // Stable identity used to key this window's resources in the engine backend.
        [[nodiscard]] WindowId id() const noexcept { return id_; }

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

        // Some changes (graphics API, GL/GLX pixel format, GPU-exclusive fullscreen, a Wayland
        // surface role change, ...) cannot be applied to a live native window on any backend —
        // the only way to change them is to destroy the window and create a new one. This
        // guarantees that ordering: `existing` is destroyed (releasing its native handle and,
        // for the last window on a backend, deinitializing the windowing library) before the
        // replacement is constructed, so the two never coexist and platform-singleton state
        // (GLFW/SDL's global init) transitions cleanly even when recreating the only window.
        // The resulting window has a new WindowId — callers must destroy and recreate whatever
        // backend resources were keyed by the old window's ID (see Engine::recreate_window()).
        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> recreate(unique_ptr<Window> existing, Args &&...args) noexcept {
            existing.reset();
            return create<Backend>(std::forward<Args>(args)...);
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

        // Returns the Vulkan instance extension strings this windowing backend requires.
        // Must be called after window creation and before the renderer backend is initialized,
        // because extensions are baked into the VkInstance at creation time. Both SDL3 and
        // GLFW return pointers into their own static storage, so the returned pointers are
        // valid for the lifetime of the backend.
        [[nodiscard]] virtual expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept = 0;

      private:
        WindowId id_;
    };

} // namespace SFT::Platform::Windowing
