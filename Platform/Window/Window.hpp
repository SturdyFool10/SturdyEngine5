#pragma once

#include <concepts>
#include <cstdint>
#include <expected>
#include <glm/vec2.hpp>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace SFT::Platform::Windowing {

    enum class WindowBackendKind {
        SDL3,
        GLFW,
    };

    enum class WindowingSystem {
        Unknown,
        SDL3,
        GLFW,
    };

    enum class WindowErrorCode {
        Unsupported,
        InvalidArgument,
        BackendUnavailable,
        CreationFailed,
        OperationFailed,
        OutOfMemory,
    };

    struct WindowError {
        WindowErrorCode code;
        std::string message;

        WindowError(WindowErrorCode error_code, const char *error_message) noexcept
            : code(error_code), message() {
            try {
                if (error_message) {
                    message = error_message;
                }
            } catch (...) {
                try {
                    message = "Window error message allocation failed.";
                } catch (...) {
                }
            }
        }

        WindowError(WindowErrorCode error_code, std::string_view error_message) noexcept
            : code(error_code), message() {
            try {
                message = error_message;
            } catch (...) {
                try {
                    message = "Window error message allocation failed.";
                } catch (...) {
                }
            }
        }
    };

    using WindowResult = std::expected<void, WindowError>;

    template <typename Value>
    using WindowExpected = std::expected<Value, WindowError>;

    using WindowExtent = glm::u32vec2;
    using WindowPosition = glm::i32vec2;

    struct WindowResize {
        WindowExtent previous = {};
        WindowExtent current = {};
        WindowExtent framebuffer = {};
        bool framebuffer_changed = false;
    };

    enum class WindowEventKind {
        CloseRequested,
        Moved,
        Resized,
        FramebufferResized,
        FocusGained,
        FocusLost,
        MouseEntered,
        MouseLeft,
        KeyPressed,
        KeyReleased,
        TextInput,
        MouseMoved,
        MouseButtonPressed,
        MouseButtonReleased,
        MouseWheel,
        MouseLocked,
        MouseUnlocked,
    };

    struct WindowKeyboardEvent {
        std::int32_t key = 0;
        std::int32_t scancode = 0;
        std::uint32_t modifiers = 0;
        bool repeat = false;
    };

    struct WindowTextInputEvent {
        char utf8[32] = {};
    };

    struct WindowMouseMoveEvent {
        float x = 0.0F;
        float y = 0.0F;
        float delta_x = 0.0F;
        float delta_y = 0.0F;
        std::uint32_t buttons = 0;
    };

    struct WindowMouseButtonEvent {
        std::uint8_t button = 0;
        std::uint8_t clicks = 1;
        float x = 0.0F;
        float y = 0.0F;
    };

    struct WindowMouseWheelEvent {
        float x = 0.0F;
        float y = 0.0F;
        float mouse_x = 0.0F;
        float mouse_y = 0.0F;
    };

    struct WindowEvent {
        WindowEventKind kind = WindowEventKind::CloseRequested;
        WindowPosition position = {};
        WindowResize resize = {};
        WindowKeyboardEvent keyboard = {};
        WindowTextInputEvent text = {};
        WindowMouseMoveEvent mouse_move = {};
        WindowMouseButtonEvent mouse_button = {};
        WindowMouseWheelEvent mouse_wheel = {};
    };

    enum class WindowMode {
        Windowed,
        BorderlessFullscreen,
        ExclusiveFullscreen,
    };

    enum class WindowGraphicsApi {
        None,
        Vulkan,
        OpenGL,
        Metal,
        Direct3D,
    };

    enum class OperatingSystem {
        Unknown,
        Windows,
        Linux,
        MacOS,
    };

    enum class NativeWindowSystem {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
    };

    struct NativeWindowHandle {
        NativeWindowSystem system = NativeWindowSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
    };

    enum class WindowEffectKind {
        Blur,
        Acrylic,
        Mica,
        MicaAlt,
        Tabbed,
        DarkMode,
        BorderColor,
        CaptionColor,
        TextColor,
    };

    enum class LinuxBlurProtocol {
        Automatic,
        ExtBackgroundEffect,
        KdeBlur,
    };

    struct WindowEffect {
        WindowEffectKind kind = WindowEffectKind::Blur;
        bool enabled = true;
        std::uint32_t color_argb = 0;
        LinuxBlurProtocol linux_blur_protocol = LinuxBlurProtocol::Automatic;

        [[nodiscard]] static constexpr WindowEffect blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect linux_ext_background_effect_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::ExtBackgroundEffect};
        }

        [[nodiscard]] static constexpr WindowEffect linux_kde_blur(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Blur, enabled, 0, LinuxBlurProtocol::KdeBlur};
        }

        [[nodiscard]] static constexpr WindowEffect acrylic(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Acrylic, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect mica(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Mica, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect mica_alt(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::MicaAlt, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect tabbed(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::Tabbed, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect dark_mode(bool enabled = true) noexcept {
            return WindowEffect{WindowEffectKind::DarkMode, enabled, 0, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect border_color(std::uint32_t color_argb) noexcept {
            return WindowEffect{WindowEffectKind::BorderColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect caption_color(std::uint32_t color_argb) noexcept {
            return WindowEffect{WindowEffectKind::CaptionColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }

        [[nodiscard]] static constexpr WindowEffect text_color(std::uint32_t color_argb) noexcept {
            return WindowEffect{WindowEffectKind::TextColor, true, color_argb, LinuxBlurProtocol::Automatic};
        }
    };

    enum class WindowEffectResultKind {
        Success,
        Degraded,
        Failed,
    };

    struct WindowEffectResult {
        WindowEffectResultKind kind = WindowEffectResultKind::Failed;
        std::string_view details = {};

        [[nodiscard]] static constexpr WindowEffectResult success(std::string_view details = {}) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Success, details};
        }

        [[nodiscard]] static constexpr WindowEffectResult degraded(std::string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Degraded, details};
        }

        [[nodiscard]] static constexpr WindowEffectResult failed(std::string_view details) noexcept {
            return WindowEffectResult{WindowEffectResultKind::Failed, details};
        }

        [[nodiscard]] constexpr bool succeeded() const noexcept {
            return kind == WindowEffectResultKind::Success || kind == WindowEffectResultKind::Degraded;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return succeeded();
        }
    };

    struct WindowConfig {
        const char *title = "Sturdy Engine";
        WindowExtent extent = {1280, 720};
        WindowPosition position = {0, 0};
        bool use_default_position = true;
        bool visible = true;
        bool resizable = true;
        bool decorated = true;
        bool high_dpi = true;
        WindowMode mode = WindowMode::Windowed;
        WindowGraphicsApi graphics_api = WindowGraphicsApi::None;
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
            requires std::derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static WindowExpected<std::unique_ptr<Backend>> create(Args &&...args) noexcept {
            try {
                return Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            } catch (const std::bad_alloc &) {
                return std::unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory while creating window."});
            } catch (...) {
                return std::unexpected(WindowError{WindowErrorCode::CreationFailed, "Unexpected exception while creating window."});
            }
        }

        [[nodiscard]] virtual WindowBackendKind backend_kind() const noexcept = 0;
        [[nodiscard]] virtual WindowingSystem type() const noexcept = 0;
        [[nodiscard]] virtual void *native_backend_handle() const noexcept = 0;
        [[nodiscard]] virtual NativeWindowHandle native_window_handle() const noexcept = 0;

        virtual WindowResult pump_events() noexcept = 0;
        [[nodiscard]] virtual std::optional<WindowEvent> poll_event() noexcept = 0;
        [[nodiscard]] virtual bool close_requested() const noexcept = 0;
        virtual void request_close() noexcept = 0;
        [[nodiscard]] virtual bool resized() const noexcept = 0;
        [[nodiscard]] virtual std::optional<WindowResize> consume_resize() noexcept = 0;

        virtual WindowResult show() noexcept = 0;
        virtual WindowResult hide() noexcept = 0;
        virtual WindowResult focus() noexcept = 0;
        virtual WindowResult raise() noexcept = 0;
        virtual WindowResult maximize() noexcept = 0;
        virtual WindowResult minimize() noexcept = 0;
        virtual WindowResult restore() noexcept = 0;

        virtual WindowResult set_title(const char *title) noexcept = 0;
        [[nodiscard]] virtual WindowExpected<WindowPosition> position() const noexcept = 0;
        virtual WindowResult set_position(WindowPosition position) noexcept = 0;
        [[nodiscard]] virtual WindowExpected<WindowExtent> size() const noexcept = 0;
        virtual WindowResult set_size(WindowExtent extent) noexcept = 0;
        [[nodiscard]] virtual WindowExpected<WindowExtent> framebuffer_size() const noexcept = 0;
        virtual WindowResult set_minimum_size(WindowExtent extent) noexcept = 0;
        virtual WindowResult set_maximum_size(WindowExtent extent) noexcept = 0;

        virtual WindowResult set_resizable(bool enabled) noexcept = 0;
        virtual WindowResult set_decorated(bool enabled) noexcept = 0;
        virtual WindowResult set_fullscreen(WindowMode mode) noexcept = 0;
        virtual WindowResult set_opacity(float opacity) noexcept = 0;
        [[nodiscard]] virtual WindowExpected<float> opacity() const noexcept = 0;

        virtual WindowResult set_cursor_visible(bool visible) noexcept = 0;
        virtual WindowResult set_cursor_grabbed(bool grabbed) noexcept = 0;
        virtual WindowResult set_relative_mouse_mode(bool enabled) noexcept = 0;
        virtual WindowResult set_mouse_locked(bool locked) noexcept = 0;
        [[nodiscard]] virtual bool mouse_locked() const noexcept = 0;

        WindowResult lock_mouse_to_window() noexcept {
            return set_mouse_locked(true);
        }

        WindowResult unlock_mouse() noexcept {
            return set_mouse_locked(false);
        }

        [[nodiscard]] WindowEffectResult enableWindowEffect(WindowEffect effect) noexcept {
            return enable_window_effect(effect);
        }

        [[nodiscard]] virtual WindowEffectResult enable_window_effect(WindowEffect effect) noexcept = 0;
        virtual WindowResult set_effect(WindowEffect effect) noexcept = 0;
        virtual WindowResult set_blur_enabled(bool enabled) noexcept = 0;
    };

} // namespace SFT::Platform::Windowing
