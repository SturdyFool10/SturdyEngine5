module;

#pragma region Imports
#include <GLFW/glfw3.h>

#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>
#pragma endregion

module Sturdy.Platform.GLFW;

#pragma region Imports
import :Window;
import Sturdy.Foundation;
import Sturdy.Platform;
#pragma endregion

using std::bad_alloc;
using std::expected;
using std::lock_guard;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::recursive_mutex;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing::GLFW {
    namespace {

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

        recursive_mutex &glfw_window_mutex() noexcept {
            static recursive_mutex mutex;
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

        GLFWWindow *window_from_glfw(GLFWwindow *window) noexcept {
            return window ? static_cast<GLFWWindow *>(glfwGetWindowUserPointer(window))
                          : nullptr;
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
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            target->events_.push_back(WindowEvent{WindowEventKind::CloseRequested});
        }
    }

    void glfw_window_pos_callback(GLFWwindow *window, int x, int y) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::Moved};
            event.position = WindowPosition{x, y};
            target->events_.push_back(event);
        }
    }

    void glfw_window_size_callback(GLFWwindow *window, int width, int height) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const WindowExtent previous = target->last_size_;
            const WindowExtent previous_framebuffer = target->last_framebuffer_size_;
            target->last_size_ =
                WindowExtent{static_cast<u32>(width),
                             static_cast<u32>(height)};

            WindowEvent event{WindowEventKind::Resized};
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
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const WindowExtent previous = target->last_size_;
            const WindowExtent previous_framebuffer = target->last_framebuffer_size_;
            target->last_framebuffer_size_ =
                WindowExtent{static_cast<u32>(width),
                             static_cast<u32>(height)};

            WindowEvent event{WindowEventKind::FramebufferResized};
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
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            target->events_.push_back(WindowEvent{
                focused == GLFW_TRUE ? WindowEventKind::FocusGained
                                     : WindowEventKind::FocusLost});
        }
    }

    void glfw_cursor_enter_callback(GLFWwindow *window, int entered) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            target->events_.push_back(WindowEvent{
                entered == GLFW_TRUE ? WindowEventKind::MouseEntered
                                     : WindowEventKind::MouseLeft});
        }
    }

    void glfw_key_callback(GLFWwindow *window, int key, int scancode, int action, int mods) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            if (action != GLFW_PRESS && action != GLFW_RELEASE &&
                action != GLFW_REPEAT) [[unlikely]] {
                return;
            }

            WindowEvent event{action == GLFW_RELEASE ? WindowEventKind::KeyReleased
                                                     : WindowEventKind::KeyPressed};
            event.keyboard = WindowKeyboardEvent{
                key,
                scancode,
                static_cast<u32>(mods),
                action == GLFW_REPEAT,
            };
            target->events_.push_back(event);
        }
    }

    void glfw_char_callback(GLFWwindow *window, unsigned int codepoint) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::TextInput};
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
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const f64 previous_x = target->has_last_mouse_position_ ? target->last_mouse_x_ : x;
            const f64 previous_y = target->has_last_mouse_position_ ? target->last_mouse_y_ : y;
            WindowEvent event{WindowEventKind::MouseMoved};
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
            event.mouse_button = WindowMouseButtonEvent{
                static_cast<u8>(button),
                1,
                static_cast<f32>(x),
                static_cast<f32>(y),
            };
            target->events_.push_back(event);
        }
    }

    void glfw_scroll_callback(GLFWwindow *window, f64 x, f64 y) {
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            f64 mouse_x = 0.0;
            f64 mouse_y = 0.0;
            glfwGetCursorPos(window, &mouse_x, &mouse_y);

            WindowEvent event{WindowEventKind::MouseWheel};
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
        const lock_guard lock(glfw_window_mutex());

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
        const lock_guard lock(glfw_window_mutex());

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
        return WindowBackendKind::GLFW;
    }

    WindowingSystem GLFWWindow::type() const noexcept {
        return WindowingSystem::GLFW;
    }

    expected<void *, WindowError> GLFWWindow::native_backend_handle() const noexcept {
        const lock_guard lock(glfw_window_mutex());
        if (!window_) [[unlikely]] {
            Foundation::log_error(
                "GLFW native backend handle query rejected destroyed window: wrapper={}",
                static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        return window_;
    }

    expected<NativeWindowHandle, WindowError> GLFWWindow::native_window_handle() const noexcept {
        const lock_guard lock(glfw_window_mutex());
        if (!window_) [[unlikely]] {
            Foundation::log_error(
                "GLFW native window handle query rejected destroyed window: wrapper={}",
                static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        auto handle = Detail::native_window_handle_from_glfw(window_);
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
        const lock_guard lock(glfw_window_mutex());
        if (auto live = require_live_window(window_, "pump_events"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_trace("GLFW poll events begin: wrapper={} native_ptr={} "
                             "close_requested_before={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested());
        glfwPollEvents();
        Foundation::log_trace("GLFW poll events complete: wrapper={} native_ptr={} "
                             "close_requested_after={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested());
        return {};
    }

    optional<WindowEvent> GLFWWindow::poll_event() noexcept {
        const lock_guard lock(glfw_window_mutex());
        if (events_.empty()) {
            return nullopt;
        }

        WindowEvent event = events_.front();
        events_.pop_front();
        return event;
    }

    bool GLFWWindow::close_requested() const noexcept {
        const lock_guard lock(glfw_window_mutex());
        if (!window_) [[unlikely]] {
            Foundation::log_warn(
                "GLFW close_requested queried after destroy: wrapper={}",
                static_cast<const void *>(this));
            return true;
        }
        return glfwWindowShouldClose(window_) == GLFW_TRUE;
    }

    void GLFWWindow::request_close() noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        events_.push_back(WindowEvent{WindowEventKind::CloseRequested});
    }

    bool GLFWWindow::resized() const noexcept {
        const lock_guard lock(glfw_window_mutex());
        return pending_resize_.has_value();
    }

    optional<WindowResize> GLFWWindow::consume_resize() noexcept {
        const lock_guard lock(glfw_window_mutex());
        optional<WindowResize> resize = pending_resize_;
        pending_resize_.reset();
        return resize;
    }

    expected<void, WindowError> GLFWWindow::show() noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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

    expected<WindowExtent, WindowError> GLFWWindow::size() const noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
        if (auto live = require_live_window(window_, "set_fullscreen"); !live) [[unlikely]] {
            return live;
        }
        if (mode == WindowMode::Windowed) [[likely]] {
            auto current_size = size();
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
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_opacity(f32 opacity) noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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

    expected<void, WindowError> GLFWWindow::set_cursor_grabbed(bool grabbed) noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        const lock_guard lock(glfw_window_mutex());
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
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_mouse_locked(bool locked) noexcept {
        const lock_guard lock(glfw_window_mutex());
        if (auto live = require_live_window(window_, "set_mouse_locked"); !live) [[unlikely]] {
            return live;
        }

        Foundation::log_debug(
            "GLFW set mouse locked: wrapper={} native_ptr={} locked={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            locked);
        glfwSetInputMode(window_, GLFW_CURSOR, locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        expected<void, WindowError> result = glfw_success();
        if (!result) [[unlikely]] {
            return result;
        }

        mouse_locked_ = locked;
        has_last_mouse_position_ = false;
        events_.push_back(
            WindowEvent{locked ? WindowEventKind::MouseLocked
                               : WindowEventKind::MouseUnlocked});
        return {};
    }

    bool GLFWWindow::mouse_locked() const noexcept {
        const lock_guard lock(glfw_window_mutex());
        return mouse_locked_;
    }

    WindowEffectResult
    GLFWWindow::enable_window_effect(WindowEffect effect) noexcept {
        const lock_guard lock(glfw_window_mutex());
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
        auto handle = native_window_handle();
        if (!handle) [[unlikely]] {
            return WindowEffectResult::failed("GLFW native window handle is unavailable.");
        }

        return enable_native_window_effect(*handle, effect);
    }

    expected<void, WindowError> GLFWWindow::set_effect(WindowEffect effect) noexcept {
        return window_result_from_effect_result(enable_window_effect(effect));
    }

    expected<void, WindowError> GLFWWindow::set_blur_enabled(bool enabled) noexcept {
        return set_effect(WindowEffect::blur(enabled));
    }

    expected<vector<const char *>, WindowError>
    GLFWWindow::required_vulkan_instance_extensions() const noexcept {
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

} // namespace SFT::Platform::Windowing::GLFW
