#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <chrono>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <variant>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Platform/Window/Window.hpp>
#include <Platform/Window/GLFW/Window.hpp>
#include <Platform/Window/GLFW/GlfwWindowNative.hpp>
#include <Platform/Platform.hpp>

#include <tracy/Tracy.hpp>

using std::bad_alloc;
using std::expected;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing::GLFW {

    expected<unique_ptr<Window>, WindowError> create_window(const WindowConfig &config) noexcept {
        ZoneScopedN("GLFW::create_window");
        auto created = Window::create<GLFWWindow>(config);
        if (!created) {
            return unexpected(created.error());
        }
        return unique_ptr<Window>{std::move(*created)};
    }

    namespace {

        [[nodiscard]] KeyboardKey keyboard_key(int key) noexcept {
            const KeyboardKey ascii_key = keyboard_key_from_ascii(key);
            if (ascii_key != KeyboardKey::Unknown) {
                return ascii_key;
            }

            switch (key) {
                case GLFW_KEY_ESCAPE: return KeyboardKey::Escape;
                case GLFW_KEY_ENTER: return KeyboardKey::Enter;
                case GLFW_KEY_TAB: return KeyboardKey::Tab;
                case GLFW_KEY_BACKSPACE: return KeyboardKey::Backspace;
                case GLFW_KEY_INSERT: return KeyboardKey::Insert;
                case GLFW_KEY_DELETE: return KeyboardKey::Delete;
                case GLFW_KEY_RIGHT: return KeyboardKey::Right;
                case GLFW_KEY_LEFT: return KeyboardKey::Left;
                case GLFW_KEY_DOWN: return KeyboardKey::Down;
                case GLFW_KEY_UP: return KeyboardKey::Up;
                case GLFW_KEY_PAGE_UP: return KeyboardKey::PageUp;
                case GLFW_KEY_PAGE_DOWN: return KeyboardKey::PageDown;
                case GLFW_KEY_HOME: return KeyboardKey::Home;
                case GLFW_KEY_END: return KeyboardKey::End;
                case GLFW_KEY_CAPS_LOCK: return KeyboardKey::CapsLock;
                case GLFW_KEY_SCROLL_LOCK: return KeyboardKey::ScrollLock;
                case GLFW_KEY_NUM_LOCK: return KeyboardKey::NumLock;
                case GLFW_KEY_PRINT_SCREEN: return KeyboardKey::PrintScreen;
                case GLFW_KEY_PAUSE: return KeyboardKey::Pause;
                case GLFW_KEY_LEFT_SHIFT: return KeyboardKey::LeftShift;
                case GLFW_KEY_LEFT_CONTROL: return KeyboardKey::LeftControl;
                case GLFW_KEY_LEFT_ALT: return KeyboardKey::LeftAlt;
                case GLFW_KEY_LEFT_SUPER: return KeyboardKey::LeftSuper;
                case GLFW_KEY_RIGHT_SHIFT: return KeyboardKey::RightShift;
                case GLFW_KEY_RIGHT_CONTROL: return KeyboardKey::RightControl;
                case GLFW_KEY_RIGHT_ALT: return KeyboardKey::RightAlt;
                case GLFW_KEY_RIGHT_SUPER: return KeyboardKey::RightSuper;
                case GLFW_KEY_MENU: return KeyboardKey::Menu;
                default: return KeyboardKey::Unknown;
            }
        }

        // Best-effort: GLFW_CURSOR_DISABLED alone still applies the OS pointer-acceleration curve to
        // the deltas GLFW synthesizes from cursor position; GLFW_RAW_MOUSE_MOTION additionally pulls
        // straight from the raw HID/evdev report where the platform supports it (SDL3's relative mode
        // gets this by default — see SDL3Impl.cpp's SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE — this is the
        // GLFW-backend equivalent). Silently skipped where unsupported, same "check before enabling
        // a best-effort hardware feature" stance as other optional-capability code in this codebase.
        void apply_raw_mouse_motion(GLFWwindow *window, bool enabled) noexcept {
            if (enabled && glfwRawMouseMotionSupported()) {
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
            } else {
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            }
        }

        WindowError glfw_error(WindowErrorCode code, const char *fallback) noexcept {
            const char *description = nullptr;
            glfwGetError(&description);
            return WindowError{code, description && description[0] != '\0' ? description : fallback};
        }

        [[nodiscard]] bool valid_extent(WindowExtent extent) noexcept {
            return extent.x > 0 && extent.y > 0 &&
                   extent.x <=
                       static_cast<u32>(numeric_limits<int>::max()) &&
                   extent.y <=
                       static_cast<u32>(numeric_limits<int>::max());
        }

        void apply_window_hints(const WindowConfig &config) noexcept {
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_DECORATED, config.decorated ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, config.transparent ? GLFW_TRUE : GLFW_FALSE);

            if (config.graphics_api == WindowGraphicsApi::OpenGL) {
                glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            } else {
                glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            }

            if (config.high_dpi) [[likely]] {
                glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
            }
        }

        [[nodiscard]] GLFWmonitor *monitor_for_mode(WindowMode mode) noexcept {
            if (mode == WindowMode::Windowed) [[likely]] {
                return nullptr;
            }

            return glfwGetPrimaryMonitor();
        }

        expected<void, WindowError> glfw_success() noexcept {
            const int code = glfwGetError(nullptr);
            if (code == GLFW_NO_ERROR) [[likely]] {
                return {};
            }

            const WindowError error =
                glfw_error(WindowErrorCode::OperationFailed, "GLFW operation failed.");
            Foundation::log_error("GLFW operation failed: glfw_code={} message='{}'", code, error.message);
            return unexpected(error);
        }

        // Process-wide, not per-window — see sdl_window_mutex()'s equivalent doc comment
        // (SDL3Impl.cpp) for why one lock covers every GLFWWindow instance, and why this is
        // Async::Mutex<std::monostate> rather than a bare mutex. Non-recursive: callers must never
        // lock it twice on the same thread (see enable_window_effect()'s use of
        // native_window_handle_locked() instead of native_window_handle(), and the equivalent
        // *_locked() helpers pump_events()/set_fullscreen() use, for exactly that reason).
        Async::Mutex<std::monostate> &glfw_window_mutex() noexcept {
            static Async::Mutex<std::monostate> mutex;
            return mutex;
        }

        int &glfw_window_count() noexcept {
            static int count = 0;
            return count;
        }

        WindowError destroyed_window_error() noexcept {
            return WindowError{WindowErrorCode::OperationFailed,
                               "GLFW window has already been destroyed."};
        }

        expected<void, WindowError> require_live_window(GLFWwindow *window,
                                                        const char *operation) noexcept {
            if (window) [[likely]] {
                return {};
            }

            Foundation::log_error(
                "GLFW operation rejected destroyed window: operation='{}'",
                operation);
            return unexpected(destroyed_window_error());
        }

        [[nodiscard]] int to_glfw_standard_cursor(CursorIcon icon) noexcept {
            switch (icon) {
                case CursorIcon::Default: return GLFW_ARROW_CURSOR;
                // GLFW has no open/closed-hand standard cursor — see CursorIcon's own doc comment
                // (Window.hpp) for why both fall back to POINTING_HAND instead of doing nothing.
                case CursorIcon::Pointer:
                case CursorIcon::Grab:
                case CursorIcon::Grabbing: return GLFW_POINTING_HAND_CURSOR;
                case CursorIcon::Text: return GLFW_IBEAM_CURSOR;
                case CursorIcon::ResizeHorizontal: return GLFW_RESIZE_EW_CURSOR;
                case CursorIcon::ResizeVertical: return GLFW_RESIZE_NS_CURSOR;
                case CursorIcon::ResizeNwse: return GLFW_RESIZE_NWSE_CURSOR;
                case CursorIcon::ResizeNesw: return GLFW_RESIZE_NESW_CURSOR;
                case CursorIcon::NotAllowed: return GLFW_NOT_ALLOWED_CURSOR;
            }
            return GLFW_ARROW_CURSOR;
        }

        GLFWWindow *window_from_glfw(GLFWwindow *window) noexcept {
            return window ? static_cast<GLFWWindow *>(glfwGetWindowUserPointer(window))
                          : nullptr;
        }

        // GLFW has no native per-event timestamp (unlike SDL3's SDL_GetTicksNS()-based one), so
        // callbacks stamp steady_clock::now() at the moment each fires -- delivery time rather than
        // hardware capture time, but callbacks run synchronously inside glfwPollEvents(), so the two
        // are close.
        [[nodiscard]] u64 steady_now_ns() noexcept {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
        }

        u32 mouse_button_state(GLFWwindow *window) noexcept {
            if (!window) [[unlikely]] {
                return 0;
            }

            u32 state = 0;
            for (int button = GLFW_MOUSE_BUTTON_1; button <= GLFW_MOUSE_BUTTON_8;
                 ++button) {
                if (glfwGetMouseButton(window, button) == GLFW_PRESS) {
                    state |= (1U << static_cast<unsigned int>(button));
                }
            }
            return state;
        }

    } // namespace

    void glfw_close_callback(GLFWwindow *window) {
        ZoneScopedN("GLFW::glfw_close_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::CloseRequested};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    void glfw_window_pos_callback(GLFWwindow *window, int x, int y) {
        ZoneScopedN("GLFW::glfw_window_pos_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::Moved};
            event.timestamp_ns = steady_now_ns();
            event.position = WindowPosition{x, y};
            target->events_.push_back(event);
        }
    }

    void glfw_window_size_callback(GLFWwindow *window, int width, int height) {
        ZoneScopedN("GLFW::glfw_window_size_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const WindowExtent previous = target->last_size_;
            const WindowExtent previous_framebuffer = target->last_framebuffer_size_;
            target->last_size_ =
                WindowExtent{static_cast<u32>(width),
                             static_cast<u32>(height)};

            WindowEvent event{WindowEventKind::Resized};
            event.timestamp_ns = steady_now_ns();
            event.resize = WindowResize{
                previous,
                target->last_size_,
                target->last_framebuffer_size_,
                previous_framebuffer.x != target->last_framebuffer_size_.x ||
                    previous_framebuffer.y != target->last_framebuffer_size_.y,
            };
            target->pending_resize_ = event.resize;
            target->events_.push_back(event);
        }
    }

    void glfw_framebuffer_size_callback(GLFWwindow *window, int width, int height) {
        ZoneScopedN("GLFW::glfw_framebuffer_size_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const WindowExtent previous = target->last_size_;
            const WindowExtent previous_framebuffer = target->last_framebuffer_size_;
            target->last_framebuffer_size_ =
                WindowExtent{static_cast<u32>(width),
                             static_cast<u32>(height)};

            WindowEvent event{WindowEventKind::FramebufferResized};
            event.timestamp_ns = steady_now_ns();
            event.resize = WindowResize{
                previous,
                target->last_size_,
                target->last_framebuffer_size_,
                previous_framebuffer.x != target->last_framebuffer_size_.x ||
                    previous_framebuffer.y != target->last_framebuffer_size_.y,
            };
            target->pending_resize_ = event.resize;
            target->events_.push_back(event);
        }
    }

    void glfw_window_focus_callback(GLFWwindow *window, int focused) {
        ZoneScopedN("GLFW::glfw_window_focus_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{focused == GLFW_TRUE ? WindowEventKind::FocusGained
                                                    : WindowEventKind::FocusLost};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    void glfw_cursor_enter_callback(GLFWwindow *window, int entered) {
        ZoneScopedN("GLFW::glfw_cursor_enter_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{entered == GLFW_TRUE ? WindowEventKind::MouseEntered
                                                    : WindowEventKind::MouseLeft};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
        ZoneScopedN("GLFW::glfw_key_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            if (action != GLFW_PRESS && action != GLFW_RELEASE &&
                action != GLFW_REPEAT) [[unlikely]] {
                return;
            }

            WindowEvent event{action == GLFW_RELEASE ? WindowEventKind::KeyReleased
                                                     : WindowEventKind::KeyPressed};
            event.timestamp_ns = steady_now_ns();
            event.keyboard = WindowKeyboardEvent{
                .key = key,
                .scancode = scancode,
                .modifiers = static_cast<u32>(mods),
                .repeat = action == GLFW_REPEAT,
                .key_code = keyboard_key(key),
            };
            target->events_.push_back(event);
        }
    }

    void glfw_char_callback(GLFWwindow *window, unsigned int codepoint) {
        ZoneScopedN("GLFW::glfw_char_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::TextInput};
            event.timestamp_ns = steady_now_ns();
            if (codepoint <= 0x7FU) [[likely]] {
                event.text.utf8[0] = static_cast<char>(codepoint);
            } else if (codepoint <= 0x7FFU) {
                event.text.utf8[0] = static_cast<char>(0xC0U | (codepoint >> 6U));
                event.text.utf8[1] = static_cast<char>(0x80U | (codepoint & 0x3FU));
            } else if (codepoint <= 0xFFFFU) {
                event.text.utf8[0] = static_cast<char>(0xE0U | (codepoint >> 12U));
                event.text.utf8[1] =
                    static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
                event.text.utf8[2] = static_cast<char>(0x80U | (codepoint & 0x3FU));
            } else if (codepoint <= 0x10FFFFU) {
                event.text.utf8[0] = static_cast<char>(0xF0U | (codepoint >> 18U));
                event.text.utf8[1] =
                    static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
                event.text.utf8[2] =
                    static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
                event.text.utf8[3] = static_cast<char>(0x80U | (codepoint & 0x3FU));
            }
            target->events_.push_back(event);
        }
    }

    void glfw_cursor_pos_callback(GLFWwindow *window, f64 x, f64 y) {
        ZoneScopedN("GLFW::glfw_cursor_pos_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const f64 previous_x = target->has_last_mouse_position_ ? target->last_mouse_x_ : x;
            const f64 previous_y = target->has_last_mouse_position_ ? target->last_mouse_y_ : y;
            WindowEvent event{WindowEventKind::MouseMoved};
            event.timestamp_ns = steady_now_ns();
            event.mouse_move = WindowMouseMoveEvent{
                static_cast<f32>(x),
                static_cast<f32>(y),
                static_cast<f32>(x - previous_x),
                static_cast<f32>(y - previous_y),
                mouse_button_state(window),
            };
            target->last_mouse_x_ = x;
            target->last_mouse_y_ = y;
            target->has_last_mouse_position_ = true;
            target->events_.push_back(event);
        }
    }

    void glfw_mouse_button_callback(GLFWwindow *window, int button, int action, int) {
        ZoneScopedN("GLFW::glfw_mouse_button_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            if (action != GLFW_PRESS && action != GLFW_RELEASE) [[unlikely]] {
                return;
            }
            f64 x = 0.0;
            f64 y = 0.0;
            glfwGetCursorPos(window, &x, &y);

            WindowEvent event{action == GLFW_PRESS
                                  ? WindowEventKind::MouseButtonPressed
                                  : WindowEventKind::MouseButtonReleased};
            event.timestamp_ns = steady_now_ns();
            event.mouse_button = WindowMouseButtonEvent{
                .button = static_cast<u8>(button),
                .clicks = 1,
                .x = static_cast<f32>(x),
                .y = static_cast<f32>(y),
                .button_code = Detail::normalize_mouse_button(button),
            };
            target->events_.push_back(event);
        }
    }

    void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y) {
        ZoneScopedN("GLFW::glfw_scroll_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            f64 mouse_x = 0.0;
            f64 mouse_y = 0.0;
            glfwGetCursorPos(window, &mouse_x, &mouse_y);

            WindowEvent event{WindowEventKind::MouseWheel};
            event.timestamp_ns = steady_now_ns();
            event.mouse_wheel = WindowMouseWheelEvent{
                static_cast<f32>(x),
                static_cast<f32>(y),
                static_cast<f32>(mouse_x),
                static_cast<f32>(mouse_y),
            };
            target->events_.push_back(event);
        }
    }

    GLFWWindow::GLFWWindow(ConstructorKey key, GLFWwindow *window) noexcept
        : Window(key), window_(window) {
        ZoneScopedN("GLFWWindow::GLFWWindow");
        if (window_) [[likely]] {
            int width = 0;
            int height = 0;
            glfwGetWindowSize(window_, &width, &height);
            last_size_ = WindowExtent{static_cast<u32>(width),
                                      static_cast<u32>(height)};
            glfwGetFramebufferSize(window_, &width, &height);
            last_framebuffer_size_ =
                WindowExtent{static_cast<u32>(width),
                             static_cast<u32>(height)};
        }
    }

    GLFWWindow::~GLFWWindow() noexcept {
        ZoneScopedN("GLFWWindow::~GLFWWindow");
        auto lock = glfw_window_mutex().lock();

        if (current_cursor_ != nullptr) {
            glfwDestroyCursor(current_cursor_);
            current_cursor_ = nullptr;
        }

        if (!window_) [[unlikely]] {
            Foundation::log_trace(
                "GLFW window destroy skipped null native pointer: wrapper={}",
                static_cast<void *>(this));
            return;
        }

        Foundation::log_info("GLFW window destroy: wrapper={} native_ptr={}",
                            static_cast<void *>(this),
                            static_cast<void *>(window_));
        glfwDestroyWindow(window_);
        window_ = nullptr;

        int &count = glfw_window_count();
        if (count > 0) [[likely]] {
            --count;
        }

        if (count == 0) {
            Foundation::log_info("GLFW terminate after last window destroyed.");
            glfwTerminate();
        } else {
            Foundation::log_debug(
                "GLFW window destroyed while other windows remain: remaining_count={}",
                count);
        }
    }

    expected<unique_ptr<GLFWWindow>, WindowError>
    GLFWWindow::construct(ConstructorKey key, const WindowConfig &config) noexcept {
        ZoneScopedN("GLFWWindow::construct");
        auto lock = glfw_window_mutex().lock();

        Foundation::log_info(
            "GLFW window create requested: title='{}' size={}x{} position=({}, {}) "
            "default_position={} visible={} resizable={} decorated={} high_dpi={} "
            "mode={} graphics_api={} existing_windows={}",
            config.title ? config.title : "<null>",
            config.extent.x,
            config.extent.y,
            config.position.x,
            config.position.y,
            config.use_default_position,
            config.visible,
            config.resizable,
            config.decorated,
            config.high_dpi,
            static_cast<int>(config.mode),
            static_cast<int>(config.graphics_api),
            glfw_window_count());

        if (!config.title || !valid_extent(config.extent)) [[unlikely]] {
            Foundation::log_error(
                "GLFW window create rejected invalid config: title_ptr={} size={}x{}",
                static_cast<const void *>(config.title),
                config.extent.x,
                config.extent.y);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument,
                                          "Invalid GLFW window configuration."});
        }

        if (config.graphics_api == WindowGraphicsApi::Metal ||
            config.graphics_api == WindowGraphicsApi::Direct3D) [[unlikely]] {
            Foundation::log_error(
                "GLFW window create rejected unsupported graphics_api={}: title='{}'",
                static_cast<int>(config.graphics_api),
                config.title);
            return unexpected(
                WindowError{WindowErrorCode::Unsupported,
                            "GLFW windows use native handles for Metal and Direct3D; "
                            "request WindowGraphicsApi::None."});
        }

        if (!glfwInit()) [[unlikely]] {
            const WindowError error = glfw_error(WindowErrorCode::BackendUnavailable,
                                                 "GLFW initialization failed.");
            Foundation::log_error("GLFW initialization failed: message='{}'",
                                 error.message);
            return unexpected(error);
        }
        Foundation::log_debug("GLFW initialized or already active.");

        apply_window_hints(config);
        Foundation::log_debug(
            "GLFW window hints applied: visible={} resizable={} decorated={} "
            "high_dpi={} client_api={}",
            config.visible,
            config.resizable,
            config.decorated,
            config.high_dpi,
            config.graphics_api == WindowGraphicsApi::OpenGL ? "OpenGL" : "NoApi");

        GLFWmonitor *monitor = monitor_for_mode(config.mode);
        Foundation::log_debug("GLFW monitor selected for create: mode={} monitor={}",
                             static_cast<int>(config.mode),
                             static_cast<void *>(monitor));
        GLFWwindow *window = glfwCreateWindow(static_cast<int>(config.extent.x),
                                              static_cast<int>(config.extent.y),
                                              config.title,
                                              monitor,
                                              nullptr);

        if (!window) [[unlikely]] {
            const WindowError error = glfw_error(WindowErrorCode::CreationFailed,
                                                 "GLFW window creation failed.");
            Foundation::log_error("glfwCreateWindow failed: title='{}' size={}x{} "
                                 "mode={} monitor={} message='{}'",
                                 config.title,
                                 config.extent.x,
                                 config.extent.y,
                                 static_cast<int>(config.mode),
                                 static_cast<void *>(monitor),
                                 error.message);
            return unexpected(error);
        }
        Foundation::log_info(
            "GLFW native window created: ptr={} title='{}' size={}x{}",
            static_cast<void *>(window),
            config.title,
            config.extent.x,
            config.extent.y);

        if (!config.use_default_position && config.mode == WindowMode::Windowed) [[unlikely]] {
            Foundation::log_debug(
                "GLFW setting initial window position: ptr={} x={} y={}",
                static_cast<void *>(window),
                config.position.x,
                config.position.y);
            glfwSetWindowPos(window, config.position.x, config.position.y);
        } else if (!config.use_default_position) [[unlikely]] {
            Foundation::log_debug(
                "GLFW skipped initial position because window is fullscreen: ptr={} "
                "mode={} requested_x={} requested_y={}",
                static_cast<void *>(window),
                static_cast<int>(config.mode),
                config.position.x,
                config.position.y);
        }

        try {
            auto wrapper = unique_ptr<GLFWWindow>(new GLFWWindow(key, window));
            // monitor_for_mode(config.mode) above already applied config.mode's GLFW-level effect to
            // the window being created — this just makes fullscreen_mode() report it accurately from
            // construction onward, matching what set_fullscreen() does for every mode change after.
            wrapper->fullscreen_mode_ = config.mode;
            glfwSetWindowUserPointer(window, wrapper.get());
            glfwSetWindowCloseCallback(window, glfw_close_callback);
            glfwSetWindowPosCallback(window, glfw_window_pos_callback);
            glfwSetWindowSizeCallback(window, glfw_window_size_callback);
            glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);
            glfwSetWindowFocusCallback(window, glfw_window_focus_callback);
            glfwSetCursorEnterCallback(window, glfw_cursor_enter_callback);
            glfwSetKeyCallback(window, glfw_key_callback);
            glfwSetCharCallback(window, glfw_char_callback);
            glfwSetCursorPosCallback(window, glfw_cursor_pos_callback);
            glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
            glfwSetScrollCallback(window, glfw_scroll_callback);
            ++glfw_window_count();
            Foundation::log_info("GLFW window wrapper constructed: wrapper={} "
                                "native_ptr={} active_count={}",
                                static_cast<void *>(wrapper.get()),
                                static_cast<void *>(window),
                                glfw_window_count());
            return wrapper;
        } catch (const bad_alloc &) {
            Foundation::log_error("GLFW window wrapper allocation failed: native_ptr={}",
                                 static_cast<void *>(window));
            glfwDestroyWindow(window);
            return unexpected(
                WindowError{WindowErrorCode::OutOfMemory,
                            "Out of memory while creating GLFW window wrapper."});
        }
    }

    WindowBackendKind GLFWWindow::backend_kind() const noexcept {
        ZoneScopedN("GLFWWindow::backend_kind");
        return WindowBackendKind::GLFW;
    }

    WindowingSystem GLFWWindow::type() const noexcept {
        ZoneScopedN("GLFWWindow::type");
        return WindowingSystem::GLFW;
    }

    expected<void *, WindowError> GLFWWindow::native_backend_handle() const noexcept {
        ZoneScopedN("GLFWWindow::native_backend_handle");
        auto lock = glfw_window_mutex().lock();
        if (!window_) [[unlikely]] {
            Foundation::log_error(
                "GLFW native backend handle query rejected destroyed window: wrapper={}",
                static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        return window_;
    }

    expected<NativeWindowHandle, WindowError> GLFWWindow::native_window_handle() const noexcept {
        ZoneScopedN("GLFWWindow::native_window_handle");
        auto lock = glfw_window_mutex().lock();
        return native_window_handle_locked();
    }

    expected<NativeWindowHandle, WindowError> GLFWWindow::native_window_handle_locked() const noexcept {
        ZoneScopedN("GLFWWindow::native_window_handle_locked");
        if (!window_) [[unlikely]] {
            Foundation::log_error(
                "GLFW native window handle query rejected destroyed window: wrapper={}",
                static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        auto handle = GLFW::Detail::native_window_handle(window_);
        if (!handle) [[unlikely]] {
            return unexpected(handle.error());
        }

        Foundation::log_debug(
            "GLFW native window handle queried: wrapper={} native_ptr={} system={} "
            "display={} window={}",
            static_cast<const void *>(this),
            static_cast<void *>(window_),
            static_cast<int>(handle->system),
            handle->display,
            handle->window);
        return *handle;
    }

    expected<void, WindowError> GLFWWindow::pump_events() noexcept {
        ZoneScopedN("GLFWWindow::pump_events");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "pump_events"); !live) [[unlikely]] {
            return live;
        }
        // close_requested_locked(), not close_requested(): this lock is already held above, and
        // glfw_window_mutex() is non-recursive — see its own doc comment.
        Foundation::log_trace("GLFW poll events begin: wrapper={} native_ptr={} "
                             "close_requested_before={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested_locked());
        glfwPollEvents();
        Foundation::log_trace("GLFW poll events complete: wrapper={} native_ptr={} "
                             "close_requested_after={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested_locked());
        return {};
    }

    optional<WindowEvent> GLFWWindow::poll_event() noexcept {
        ZoneScopedN("GLFWWindow::poll_event");
        auto lock = glfw_window_mutex().lock();
        if (events_.empty()) {
            return nullopt;
        }

        WindowEvent event = events_.front();
        events_.pop_front();
        return event;
    }

    bool GLFWWindow::close_requested() const noexcept {
        ZoneScopedN("GLFWWindow::close_requested");
        auto lock = glfw_window_mutex().lock();
        return close_requested_locked();
    }

    bool GLFWWindow::close_requested_locked() const noexcept {
        ZoneScopedN("GLFWWindow::close_requested_locked");
        if (!window_) [[unlikely]] {
            Foundation::log_warn(
                "GLFW close_requested queried after destroy: wrapper={}",
                static_cast<const void *>(this));
            return true;
        }
        return glfwWindowShouldClose(window_) == GLFW_TRUE;
    }

    void GLFWWindow::request_close() noexcept {
        ZoneScopedN("GLFWWindow::request_close");
        auto lock = glfw_window_mutex().lock();
        if (!window_) [[unlikely]] {
            Foundation::log_warn(
                "GLFW request close ignored destroyed window: wrapper={}",
                static_cast<void *>(this));
            return;
        }
        Foundation::log_warn(
            "GLFW close requested by engine: wrapper={} native_ptr={}",
            static_cast<void *>(this),
            static_cast<void *>(window_));
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
        WindowEvent close_event{WindowEventKind::CloseRequested};
        close_event.timestamp_ns = steady_now_ns();
        events_.push_back(close_event);
    }

    bool GLFWWindow::resized() const noexcept {
        ZoneScopedN("GLFWWindow::resized");
        auto lock = glfw_window_mutex().lock();
        return pending_resize_.has_value();
    }

    optional<WindowResize> GLFWWindow::consume_resize() noexcept {
        ZoneScopedN("GLFWWindow::consume_resize");
        auto lock = glfw_window_mutex().lock();
        optional<WindowResize> resize = pending_resize_;
        pending_resize_.reset();
        return resize;
    }

    expected<void, WindowError> GLFWWindow::show() noexcept {
        ZoneScopedN("GLFWWindow::show");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "show"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW show window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwShowWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::hide() noexcept {
        ZoneScopedN("GLFWWindow::hide");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "hide"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW hide window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwHideWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::focus() noexcept {
        ZoneScopedN("GLFWWindow::focus");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "focus"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW focus window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwFocusWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::raise() noexcept {
        ZoneScopedN("GLFWWindow::raise");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "raise"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW request window attention: wrapper={} native_ptr={}",
            static_cast<void *>(this),
            static_cast<void *>(window_));
        glfwRequestWindowAttention(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::maximize() noexcept {
        ZoneScopedN("GLFWWindow::maximize");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "maximize"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW maximize window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwMaximizeWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::minimize() noexcept {
        ZoneScopedN("GLFWWindow::minimize");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "minimize"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW minimize/iconify window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwIconifyWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::restore() noexcept {
        ZoneScopedN("GLFWWindow::restore");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "restore"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW restore window: wrapper={} native_ptr={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_));
        glfwRestoreWindow(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_title(const char *title) noexcept {
        ZoneScopedN("GLFWWindow::set_title");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_title"); !live) [[unlikely]] {
            return live;
        }
        if (!title) [[unlikely]] {
            Foundation::log_error(
                "GLFW set title rejected null title: wrapper={} native_ptr={}",
                static_cast<void *>(this),
                static_cast<void *>(window_));
            return unexpected(WindowError{WindowErrorCode::InvalidArgument,
                                          "Window title cannot be null."});
        }

        glfwSetWindowTitle(window_, title);
        return glfw_success();
    }

    expected<WindowPosition, WindowError> GLFWWindow::position() const noexcept {
        ZoneScopedN("GLFWWindow::position");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "position"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        int x = 0;
        int y = 0;
        glfwGetWindowPos(window_, &x, &y);
        Foundation::log_trace("GLFW get position: wrapper={} native_ptr={} x={} y={}",
                             static_cast<const void *>(this),
                             static_cast<void *>(window_),
                             x,
                             y);
        return WindowPosition{x, y};
    }

    expected<void, WindowError> GLFWWindow::set_position(WindowPosition position) noexcept {
        ZoneScopedN("GLFWWindow::set_position");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_position"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("GLFW set position: wrapper={} native_ptr={} x={} y={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             position.x,
                             position.y);
        glfwSetWindowPos(window_, position.x, position.y);
        return glfw_success();
    }

    expected<WindowPosition, WindowError> GLFWWindow::global_cursor_position() const noexcept {
        ZoneScopedN("GLFWWindow::global_cursor_position");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "global_cursor_position"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
#if defined(GLFW_PLATFORM_WAYLAND)
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return unexpected(WindowError{
                WindowErrorCode::Unsupported,
                "GLFW global cursor position is unavailable on Wayland.",
            });
        }
#endif

        // GLFW has no native global-desktop cursor query (unlike SDL3's SDL_GetGlobalMouseState),
        // so supported desktop backends derive it from the window position plus the window-relative
        // cursor position. During a held drag, native mouse capture keeps that relative position
        // updating after the cursor leaves the client area on Win32 and X11.
        f64 x = 0.0;
        f64 y = 0.0;
        glfwGetCursorPos(window_, &x, &y);
        int window_x = 0;
        int window_y = 0;
        glfwGetWindowPos(window_, &window_x, &window_y);
        return WindowPosition{window_x + static_cast<i32>(x), window_y + static_cast<i32>(y)};
    }

    expected<WindowExtent, WindowError> GLFWWindow::size() const noexcept {
        ZoneScopedN("GLFWWindow::size");
        auto lock = glfw_window_mutex().lock();
        return size_locked();
    }

    expected<WindowExtent, WindowError> GLFWWindow::size_locked() const noexcept {
        ZoneScopedN("GLFWWindow::size_locked");
        if (auto live = require_live_window(window_, "size"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        int width = 0;
        int height = 0;
        glfwGetWindowSize(window_, &width, &height);
        Foundation::log_trace(
            "GLFW get size: wrapper={} native_ptr={} width={} height={}",
            static_cast<const void *>(this),
            static_cast<void *>(window_),
            width,
            height);
        return WindowExtent{static_cast<u32>(width),
                            static_cast<u32>(height)};
    }

    expected<void, WindowError> GLFWWindow::set_size(WindowExtent extent) noexcept {
        ZoneScopedN("GLFWWindow::set_size");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("GLFW set size rejected invalid extent: wrapper={} "
                                 "native_ptr={} width={} height={}",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 extent.x,
                                 extent.y);
            return unexpected(
                WindowError{WindowErrorCode::InvalidArgument,
                            "Window size must be positive and fit in an int."});
        }

        Foundation::log_debug(
            "GLFW set size: wrapper={} native_ptr={} width={} height={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            extent.x,
            extent.y);
        glfwSetWindowSize(window_, static_cast<int>(extent.x), static_cast<int>(extent.y));
        return glfw_success();
    }

    expected<WindowExtent, WindowError> GLFWWindow::framebuffer_size() const noexcept {
        ZoneScopedN("GLFWWindow::framebuffer_size");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "framebuffer_size"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        Foundation::log_trace(
            "GLFW get framebuffer size: wrapper={} native_ptr={} width={} height={}",
            static_cast<const void *>(this),
            static_cast<void *>(window_),
            width,
            height);
        return WindowExtent{static_cast<u32>(width),
                            static_cast<u32>(height)};
    }

    expected<void, WindowError> GLFWWindow::set_minimum_size(WindowExtent extent) noexcept {
        ZoneScopedN("GLFWWindow::set_minimum_size");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_minimum_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("GLFW set minimum size rejected invalid extent: "
                                 "wrapper={} native_ptr={} width={} height={}",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 extent.x,
                                 extent.y);
            return unexpected(
                WindowError{WindowErrorCode::InvalidArgument,
                            "Minimum size must be positive and fit in an int."});
        }

        Foundation::log_debug(
            "GLFW set minimum size: wrapper={} native_ptr={} width={} height={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            extent.x,
            extent.y);
        glfwSetWindowSizeLimits(window_, static_cast<int>(extent.x), static_cast<int>(extent.y), GLFW_DONT_CARE, GLFW_DONT_CARE);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_maximum_size(WindowExtent extent) noexcept {
        ZoneScopedN("GLFWWindow::set_maximum_size");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_maximum_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("GLFW set maximum size rejected invalid extent: "
                                 "wrapper={} native_ptr={} width={} height={}",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 extent.x,
                                 extent.y);
            return unexpected(
                WindowError{WindowErrorCode::InvalidArgument,
                            "Maximum size must be positive and fit in an int."});
        }

        Foundation::log_debug(
            "GLFW set maximum size: wrapper={} native_ptr={} width={} height={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            extent.x,
            extent.y);
        glfwSetWindowSizeLimits(window_, GLFW_DONT_CARE, GLFW_DONT_CARE, static_cast<int>(extent.x), static_cast<int>(extent.y));
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_resizable(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_resizable");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_resizable"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW set resizable: wrapper={} native_ptr={} enabled={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            enabled);
        glfwSetWindowAttrib(window_, GLFW_RESIZABLE, enabled ? GLFW_TRUE : GLFW_FALSE);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_decorated(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_decorated");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_decorated"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW set decorated: wrapper={} native_ptr={} enabled={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            enabled);
        glfwSetWindowAttrib(window_, GLFW_DECORATED, enabled ? GLFW_TRUE : GLFW_FALSE);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_fullscreen(WindowMode mode) noexcept {
        ZoneScopedN("GLFWWindow::set_fullscreen");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_fullscreen"); !live) [[unlikely]] {
            return live;
        }
        if (mode == WindowMode::Windowed) [[likely]] {
            // size_locked(), not size(): this lock is already held above, and glfw_window_mutex() is
            // non-recursive — see its own doc comment.
            auto current_size = size_locked();
            const WindowExtent extent = current_size ? *current_size : WindowExtent{};
            Foundation::log_info(
                "GLFW set fullscreen/windowed: wrapper={} native_ptr={} mode={} "
                "restored_width={} restored_height={}",
                static_cast<void *>(this),
                static_cast<void *>(window_),
                static_cast<int>(mode),
                extent.x,
                extent.y);
            glfwSetWindowMonitor(window_, nullptr, 0, 0, static_cast<int>(extent.x), static_cast<int>(extent.y), GLFW_DONT_CARE);
            fullscreen_mode_ = mode;
            return glfw_success();
        }

        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (!monitor) [[unlikely]] {
            Foundation::log_error("GLFW set fullscreen failed missing primary monitor: "
                                 "wrapper={} native_ptr={} mode={}",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 static_cast<int>(mode));
            return unexpected(WindowError{WindowErrorCode::OperationFailed,
                                          "GLFW primary monitor is unavailable."});
        }

        const GLFWvidmode *mode_info = glfwGetVideoMode(monitor);
        if (!mode_info) [[unlikely]] {
            const WindowError error = glfw_error(WindowErrorCode::OperationFailed,
                                                 "GLFW video mode is unavailable.");
            Foundation::log_error("GLFW set fullscreen failed missing video mode: "
                                 "wrapper={} native_ptr={} monitor={} message='{}'",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 static_cast<void *>(monitor),
                                 error.message);
            return unexpected(error);
        }

        Foundation::log_info("GLFW set fullscreen: wrapper={} native_ptr={} mode={} "
                            "monitor={} width={} height={} refresh_rate={}",
                            static_cast<void *>(this),
                            static_cast<void *>(window_),
                            static_cast<int>(mode),
                            static_cast<void *>(monitor),
                            mode_info->width,
                            mode_info->height,
                            mode_info->refreshRate);
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode_info->width, mode_info->height, mode_info->refreshRate);
        // GLFW, like SDL3, has no notion of "exclusive" distinct from a monitor-covering windowed-
        // fullscreen window at this layer — see fullscreen_mode()'s own doc comment (Window.hpp) for
        // why the requested mode is still recorded precisely rather than collapsed here.
        fullscreen_mode_ = mode;
        return glfw_success();
    }

    WindowMode GLFWWindow::fullscreen_mode() const noexcept {
        return fullscreen_mode_;
    }

    expected<void, WindowError> GLFWWindow::set_opacity(f32 opacity) noexcept {
        ZoneScopedN("GLFWWindow::set_opacity");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_opacity"); !live) [[unlikely]] {
            return live;
        }
        if (opacity < 0.0F || opacity > 1.0F) [[unlikely]] {
            Foundation::log_error("GLFW set opacity rejected invalid opacity: "
                                 "wrapper={} native_ptr={} opacity={}",
                                 static_cast<void *>(this),
                                 static_cast<void *>(window_),
                                 opacity);
            return unexpected(
                WindowError{WindowErrorCode::InvalidArgument,
                            "Window opacity must be between 0.0 and 1.0."});
        }
        Foundation::log_debug("GLFW set opacity: wrapper={} native_ptr={} opacity={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             opacity);
        glfwSetWindowOpacity(window_, opacity);
        return glfw_success();
    }

    expected<f32, WindowError> GLFWWindow::opacity() const noexcept {
        ZoneScopedN("GLFWWindow::opacity");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "opacity"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        const f32 result = glfwGetWindowOpacity(window_);
        Foundation::log_trace("GLFW get opacity: wrapper={} native_ptr={} opacity={}",
                             static_cast<const void *>(this),
                             static_cast<void *>(window_),
                             result);
        return result;
    }

    expected<void, WindowError> GLFWWindow::set_cursor_visible(bool visible) noexcept {
        ZoneScopedN("GLFWWindow::set_cursor_visible");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_visible"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW set cursor visible: wrapper={} native_ptr={} visible={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            visible);
        glfwSetInputMode(window_, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_cursor_icon(CursorIcon icon) noexcept {
        ZoneScopedN("GLFWWindow::set_cursor_icon");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_icon"); !live) [[unlikely]] {
            return live;
        }
        // Skip the churn entirely on the overwhelmingly common case (nothing changed since last
        // call) — a caller driving this off per-frame hover state, as UI::Context::desired_cursor()
        // is meant to be, would otherwise recreate a native cursor object every single frame.
        if (current_cursor_icon_.has_value() && *current_cursor_icon_ == icon) {
            return glfw_success();
        }
        GLFWcursor *cursor = glfwCreateStandardCursor(to_glfw_standard_cursor(icon));
        if (cursor == nullptr) [[unlikely]] {
            Foundation::log_error(
                "GLFW create standard cursor failed: wrapper={} native_ptr={} icon={}",
                static_cast<void *>(this), static_cast<void *>(window_), static_cast<i32>(icon));
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW create standard cursor failed."});
        }
        glfwSetCursor(window_, cursor);
        if (current_cursor_ != nullptr) {
            glfwDestroyCursor(current_cursor_);
        }
        current_cursor_ = cursor;
        current_cursor_icon_ = icon;
        Foundation::log_trace(
            "GLFW set cursor icon: wrapper={} native_ptr={} icon={}",
            static_cast<void *>(this), static_cast<void *>(window_), static_cast<i32>(icon));
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_cursor_grabbed(bool grabbed) noexcept {
        ZoneScopedN("GLFWWindow::set_cursor_grabbed");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_grabbed"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW set cursor grabbed: wrapper={} native_ptr={} grabbed={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            grabbed);
        glfwSetInputMode(window_, GLFW_CURSOR, grabbed ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_relative_mouse_mode(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_relative_mouse_mode");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_relative_mouse_mode");
            !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug(
            "GLFW set relative mouse mode: wrapper={} native_ptr={} enabled={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            enabled);
        glfwSetInputMode(window_, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        apply_raw_mouse_motion(window_, enabled);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_mouse_locked(bool locked) noexcept {
        ZoneScopedN("GLFWWindow::set_mouse_locked");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_mouse_locked"); !live) [[unlikely]] {
            return live;
        }

        Foundation::log_debug(
            "GLFW set mouse locked: wrapper={} native_ptr={} locked={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            locked);
        glfwSetInputMode(window_, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        apply_raw_mouse_motion(window_, locked);
        expected<void, WindowError> result = glfw_success();
        if (!result) [[unlikely]] {
            return result;
        }

        mouse_locked_ = locked;
        has_last_mouse_position_ = false;
        WindowEvent lock_event{locked ? WindowEventKind::MouseLocked
                                      : WindowEventKind::MouseUnlocked};
        lock_event.timestamp_ns = steady_now_ns();
        events_.push_back(lock_event);
        return {};
    }

    bool GLFWWindow::mouse_locked() const noexcept {
        ZoneScopedN("GLFWWindow::mouse_locked");
        auto lock = glfw_window_mutex().lock();
        return mouse_locked_;
    }

    WindowEffectResult
    GLFWWindow::enable_window_effect(WindowEffect effect) noexcept {
        ZoneScopedN("GLFWWindow::enable_window_effect");
        auto lock = glfw_window_mutex().lock();
        if (!window_) [[unlikely]] {
            Foundation::log_error("GLFW enable native window effect rejected destroyed "
                                 "window: wrapper={} kind={}",
                                 static_cast<void *>(this),
                                 static_cast<int>(effect.kind));
            return WindowEffectResult::failed(
                "GLFW window has already been destroyed.");
        }
        Foundation::log_info(
            "GLFW enable native window effect: wrapper={} native_ptr={} kind={} "
            "enabled={} color_argb=0x{:08X} linux_blur_protocol={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb,
            static_cast<int>(effect.linux_blur_protocol));
        // native_window_handle_locked(), not native_window_handle(): this lock is already held
        // above, and glfw_window_mutex() is non-recursive — see its own doc comment.
        auto handle = native_window_handle_locked();
        if (!handle) [[unlikely]] {
            return WindowEffectResult::failed("GLFW native window handle is unavailable.");
        }

        return enable_native_window_effect(*handle, effect);
    }

    expected<void, WindowError> GLFWWindow::set_effect(WindowEffect effect) noexcept {
        ZoneScopedN("GLFWWindow::set_effect");
        return window_result_from_effect_result(enable_window_effect(effect));
    }

    expected<void, WindowError> GLFWWindow::set_blur_enabled(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_blur_enabled");
        return set_effect(WindowEffect::blur(enabled));
    }

    expected<void, WindowError> GLFWWindow::set_transparent(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_transparent");
        return set_effect(WindowEffect::transparent(enabled));
    }

    expected<vector<const char *>, WindowError>
    GLFWWindow::required_vulkan_instance_extensions() const noexcept {
        ZoneScopedN("GLFWWindow::required_vulkan_instance_extensions");
        u32 count = 0;
        const char **extensions = glfwGetRequiredInstanceExtensions(&count);
        if (!extensions) {
            return unexpected(glfw_error(WindowErrorCode::OperationFailed,
                                         "glfwGetRequiredInstanceExtensions failed; "
                                         "Vulkan may not be supported by GLFW on this system."));
        }
        try {
            return vector<const char *>{extensions, extensions + count};
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory,
                                          "Out of memory querying Vulkan WSI extensions."});
        }
    }

    expected<void, WindowError> GLFWWindow::create_vulkan_surface(
        void *instance,
        const void *allocation_callbacks,
        void *surface_out) const noexcept {
        ZoneScopedN("GLFWWindow::create_vulkan_surface");
        auto lock = glfw_window_mutex().lock();
        if (!window_ || instance == nullptr || surface_out == nullptr) {
            return unexpected(WindowError{
                WindowErrorCode::InvalidArgument,
                "GLFW Vulkan surface creation requires a live window, instance, and output pointer.",
            });
        }
        const auto result = glfwCreateWindowSurface(
            static_cast<VkInstance>(instance),
            window_,
            static_cast<const VkAllocationCallbacks *>(allocation_callbacks),
            static_cast<VkSurfaceKHR *>(surface_out));
        if (result != VK_SUCCESS) {
            return unexpected(WindowError{WindowErrorCode::CreationFailed,
                                          "glfwCreateWindowSurface failed."});
        }
        return {};
    }

    std::string GLFWWindow::clipboard_text() const noexcept {
        ZoneScopedN("GLFWWindow::clipboard_text");
        const char *text = glfwGetClipboardString(window_);
        return text ? std::string(text) : std::string();
    }

    expected<void, WindowError> GLFWWindow::set_clipboard_text(std::string_view text) noexcept {
        ZoneScopedN("GLFWWindow::set_clipboard_text");
        // glfwSetClipboardString requires a NUL-terminated C string; `text` (a view) isn't
        // guaranteed to be one, so it's copied into an owned buffer first.
        const std::string owned(text);
        glfwSetClipboardString(window_, owned.c_str());
        return {};
    }

} // namespace SFT::Platform::Windowing::GLFW
