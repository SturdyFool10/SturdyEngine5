#include <Foundation/Foundation.hpp>

#pragma region Imports
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <variant>
#include <vector>
#pragma endregion

#include <Async/Mutex.hpp>
#include <WindowManager/Desktop.hpp>
#include <WindowManager/Window.hpp>
#include <WindowManager/WindowTextUtil.hpp>
#include <WindowManager/Providers/GLFW/Window.hpp>
#include <WindowManager/Providers/GLFW/GlfwWindowNative.hpp>
#include <WindowManager/WindowManager.hpp>
#if defined(__linux__)
#include <WindowManager/Linux/WaylandColorManagement.hpp>
#endif

#include <tracy/Tracy.hpp>

using std::bad_alloc;
using std::expected;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::WindowManager::GLFW {

    /// Creates a window from the supplied parameters.
    ///
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<unique_ptr<Window>, WindowError> create_window(const WindowConfig &config) noexcept {
        ZoneScopedN("GLFW::create_window");
        auto created = Window::create<GLFWWindow>(config);
        if (!created) {
            return unexpected(created.error());
        }
        return unique_ptr<Window>{std::move(*created)};
    }

    namespace {

        /// Performs the keyboard key operation for `GLFW` using the supplied arguments.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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


        /// Applies raw mouse motion using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void apply_raw_mouse_motion(GLFWwindow *window, bool enabled) noexcept {
            if (enabled && glfwRawMouseMotionSupported()) {
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
            } else {
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            }
        }

        /// Creates an error result describing the supplied GLFW failure.
        ///
        /// @param code `code` value used by the operation.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        WindowError glfw_error(WindowErrorCode code, const char *fallback) noexcept {
            const char *description = nullptr;
            glfwGetError(&description);
            return WindowError{code, description && description[0] != '\0' ? description : fallback};
        }

        /// Reports whether valid extent is valid for the current operation.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool valid_extent(WindowExtent extent) noexcept {
            return extent.x > 0 && extent.y > 0 &&
                   extent.x <=
                       static_cast<u32>(numeric_limits<int>::max()) &&
                   extent.y <=
                       static_cast<u32>(numeric_limits<int>::max());
        }

        /// Applies window hints using the supplied arguments and current state.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply_window_hints(const WindowConfig &config) noexcept {
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_DECORATED, config.decorated ? GLFW_TRUE : GLFW_FALSE);
            glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, config.transparent ? GLFW_TRUE : GLFW_FALSE);

            // The Wayland app_id / X11 WM_CLASS that desktop files and Flatpak match a window to (same as SDL3's
            // SDL_HINT_APP_ID). Unlike SDL's it is per window, not process-wide.
            if (config.app_id != nullptr && config.app_id[0] != '\0') {
                glfwWindowHintString(GLFW_WAYLAND_APP_ID, config.app_id);
                glfwWindowHintString(GLFW_X11_CLASS_NAME, config.app_id);
                glfwWindowHintString(GLFW_X11_INSTANCE_NAME, config.app_id);
            }

            if (config.graphics_api == WindowGraphicsApi::OpenGL) {
                glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            } else {
                glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            }

            // WindowConfig::extent is physical pixels, so never let GLFW grow the window by the
            // monitor's content scale (GLFW_SCALE_TO_MONITOR, Win32/X11). GLFW_SCALE_FRAMEBUFFER
            // is what controls a full-resolution framebuffer on macOS/Wayland.
            glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
            glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, config.high_dpi ? GLFW_TRUE : GLFW_FALSE);
        }

#if defined(_WIN32)
        /// Reports whether `config` will end up on the DirectComposition path at all.
        ///
        /// @param config Window configuration to check.
        ///
        /// @return true for every Vulkan/Direct3D window, not only ones with `config.transparent`
        ///         set — both graphics backends try DirectComposition for every swapchain generation
        ///         they create on such a window (see VulkanRhiBridgeSwapchain.cpp's create_swapchain()
        ///         and D3D12DeviceSwapchain.cpp's wants_transparency branch, both of which attempt
        ///         CreateSwapChainForComposition unconditionally), so this has to match that same
        ///         condition — see GLFW::Detail::apply_composition_window_style's own doc comment for
        ///         why every such window needs WS_EX_NOREDIRECTIONBITMAP applied.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wants_composition_window_style(const WindowConfig &config) noexcept {
            return config.graphics_api == WindowGraphicsApi::Vulkan ||
                   config.graphics_api == WindowGraphicsApi::Direct3D;
        }
#endif

        /// Performs the monitor for mode operation for `GLFW` using the supplied arguments.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] GLFWmonitor *monitor_for_mode(WindowMode mode) noexcept {
            if (mode == WindowMode::Windowed) [[likely]] {
                return nullptr;
            }

            return glfwGetPrimaryMonitor();
        }

        /// The display id (`DisplayInfo::id`) GLFW windows report for a monitor: its 1-based index in `glfwGetMonitors`.
        [[nodiscard]] GLFWmonitor *monitor_for_display_id(u32 display_id) noexcept {
            int count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&count);
            if (monitors == nullptr || display_id == 0 || display_id > static_cast<u32>(count)) {
                return nullptr;
            }
            return monitors[display_id - 1];
        }

        /// The monitor a window is on: the one containing the centre of the window, else the primary monitor.
        [[nodiscard]] GLFWmonitor *monitor_containing(GLFWwindow *window) noexcept {
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            glfwGetWindowPos(window, &x, &y);
            glfwGetWindowSize(window, &width, &height);
            const int center_x = x + width / 2;
            const int center_y = y + height / 2;
            int count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&count);
            for (int i = 0; monitors != nullptr && i < count; ++i) {
                int monitor_x = 0;
                int monitor_y = 0;
                glfwGetMonitorPos(monitors[i], &monitor_x, &monitor_y);
                if (const GLFWvidmode *mode = glfwGetVideoMode(monitors[i])) {
                    if (center_x >= monitor_x && center_x < monitor_x + mode->width && center_y >= monitor_y &&
                        center_y < monitor_y + mode->height) {
                        return monitors[i];
                    }
                }
            }
            return glfwGetPrimaryMonitor();
        }

        /// The monitor a fullscreen request lands on.
        [[nodiscard]] GLFWmonitor *monitor_for_target(GLFWwindow *window, const FullscreenTarget &target) noexcept {
            if (target.display_id.has_value()) {
                if (GLFWmonitor *monitor = monitor_for_display_id(*target.display_id)) {
                    return monitor;
                }
            }
            return monitor_containing(window);
        }

        /// Returns the current or globally available GLFW success value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
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


        /// Returns the current or globally available GLFW window mutex value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        Async::Mutex<std::monostate> &glfw_window_mutex() noexcept {
            static Async::Mutex<std::monostate> mutex;
            return mutex;
        }

        /// Returns the GLFW window count for this `GLFW`.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        int &glfw_window_count() noexcept {
            static int count = 0;
            return count;
        }

        /// Every live GLFW window, for events that belong to the whole process (monitors connecting). Guarded by
        /// `glfw_window_mutex()`, like everything else here.
        std::vector<GLFWWindow *> &glfw_live_windows() noexcept {
            static std::vector<GLFWWindow *> windows;
            return windows;
        }

        /// Creates an error result describing the supplied destroyed window failure.
        ///
        /// @return Returns the current destroyed window error value.
        /// @note This function does not throw exceptions.
        WindowError destroyed_window_error() noexcept {
            return WindowError{WindowErrorCode::OperationFailed,
                               "GLFW window has already been destroyed."};
        }

        /// Performs the require live window operation for `GLFW` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
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

        /// Converts the value to GLFW standard cursor representation.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @return Returns the value converted to GLFW standard cursor representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] int to_glfw_standard_cursor(CursorIcon icon) noexcept {
            switch (icon) {
                case CursorIcon::Default: return GLFW_ARROW_CURSOR;


                case CursorIcon::Pointer:
                case CursorIcon::Grab:
                case CursorIcon::Grabbing: return GLFW_POINTING_HAND_CURSOR;
                case CursorIcon::Text: return GLFW_IBEAM_CURSOR;
                case CursorIcon::ResizeHorizontal: return GLFW_RESIZE_EW_CURSOR;
                case CursorIcon::ResizeVertical: return GLFW_RESIZE_NS_CURSOR;
                case CursorIcon::ResizeNwse: return GLFW_RESIZE_NWSE_CURSOR;
                case CursorIcon::ResizeNesw: return GLFW_RESIZE_NESW_CURSOR;
                case CursorIcon::NotAllowed: return GLFW_NOT_ALLOWED_CURSOR;
                case CursorIcon::Crosshair: return GLFW_CROSSHAIR_CURSOR;
                case CursorIcon::Move: return GLFW_RESIZE_ALL_CURSOR;
                // GLFW has only the four axis arrows, so an edge or corner shows the axis it lies on.
                case CursorIcon::ResizeN:
                case CursorIcon::ResizeS: return GLFW_RESIZE_NS_CURSOR;
                case CursorIcon::ResizeE:
                case CursorIcon::ResizeW: return GLFW_RESIZE_EW_CURSOR;
                case CursorIcon::ResizeNe:
                case CursorIcon::ResizeSw: return GLFW_RESIZE_NESW_CURSOR;
                case CursorIcon::ResizeNw:
                case CursorIcon::ResizeSe: return GLFW_RESIZE_NWSE_CURSOR;
                // GLFW has no busy cursors; the caller turns -1 into `Unsupported` rather than showing the wrong one.
                case CursorIcon::Wait:
                case CursorIcon::Progress: return -1;
            }
            return GLFW_ARROW_CURSOR;
        }

        /// Performs the window from GLFW operation for `GLFW` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        GLFWWindow *window_from_glfw(GLFWwindow *window) noexcept {
            return window ? static_cast<GLFWWindow *>(glfwGetWindowUserPointer(window))
                          : nullptr;
        }


        /// Returns the current or globally available steady now ns value.
        ///
        /// @return Returns the current steady now ns value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 steady_now_ns() noexcept {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
        }

        /// Performs the mouse button state operation for `GLFW` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Reads the window's current framebuffer-to-window ratio.
        ///
        /// @param window Live GLFW window.
        ///
        /// @return Physical pixels per screen coordinate.
        [[nodiscard]] glm::vec2 glfw_pixel_density(GLFWwindow *window) noexcept {
            int width = 0;
            int height = 0;
            int fb_width = 0;
            int fb_height = 0;
            glfwGetWindowSize(window, &width, &height);
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
            if (width <= 0 || height <= 0 || fb_width <= 0 || fb_height <= 0) {
                return glm::vec2{1.0f};
            }
            return pixel_density(WindowExtent{static_cast<u32>(width), static_cast<u32>(height)},
                                 WindowExtent{static_cast<u32>(fb_width), static_cast<u32>(fb_height)});
        }

        /// Converts a physical-pixel extent into GLFW screen coordinates using the window's current
        /// framebuffer-to-window ratio (1 on Win32/X11, the output scale on macOS/Wayland).
        ///
        /// @param window Live GLFW window.
        /// @param extent Extent in physical pixels.
        ///
        /// @return The extent in screen coordinates.
        [[nodiscard]] WindowExtent glfw_window_extent_from_physical(GLFWwindow *window, WindowExtent extent) noexcept {
            return physical_to_window_extent(extent, glfw_pixel_density(window));
        }

        /// Reads the window's content scale (x axis; GLFW reports both but they match on every platform).
        ///
        /// @param window Live GLFW window.
        ///
        /// @return The content scale, or 1.0 when GLFW reports nothing usable.
        [[nodiscard]] f32 glfw_content_scale(GLFWwindow *window) noexcept {
            float x_scale = 1.0f;
            float y_scale = 1.0f;
            glfwGetWindowContentScale(window, &x_scale, &y_scale);
            return x_scale > 0.0f ? x_scale : 1.0f;
        }

    } // namespace

    /// Handles the GLFW close callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_close_callback(GLFWwindow *window) {
        ZoneScopedN("GLFW::glfw_close_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::CloseRequested};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    /// Handles the GLFW window pos callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_pos_callback(GLFWwindow *window, int x, int y) {
        ZoneScopedN("GLFW::glfw_window_pos_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::Moved};
            event.timestamp_ns = steady_now_ns();
            event.position = WindowPosition{x, y};
            target->events_.push_back(event);
        }
    }

    /// Handles the GLFW window size callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_size_callback(GLFWwindow *window, int width, int height) {
        ZoneScopedN("GLFW::glfw_window_size_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
#if defined(_WIN32)
            // Same contract as SDL3's event watch: only Windows runs a modal resize loop that blocks the poll loop, so
            // only there is this callback the sole chance to redraw while the border is dragged. GLFW's size callback
            // does fire from inside that loop. Everywhere else the normal event path redraws. Pixels, not window units.
            if (target->live_resize_callback_) {
                int framebuffer_width = 0;
                int framebuffer_height = 0;
                glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
                const WindowExtent pixels{static_cast<u32>(std::max(framebuffer_width, 0)), static_cast<u32>(std::max(framebuffer_height, 0))};
                if (pixels.x > 0 && pixels.y > 0 &&
                    (pixels.x != target->last_live_resize_extent_.x || pixels.y != target->last_live_resize_extent_.y)) {
                    target->last_live_resize_extent_ = pixels;
                    target->live_resize_callback_(pixels);
                }
            }
#endif
            target->reapply_size_dependent_effects_locked();

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

    /// GLFW reports maximise and restore separately from iconify; both map onto the provider-neutral events.
    void glfw_window_maximize_callback(GLFWwindow *window, int maximized) {
        ZoneScopedN("GLFW::glfw_window_maximize_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{maximized == GLFW_TRUE ? WindowEventKind::Maximized : WindowEventKind::Restored};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    void glfw_window_iconify_callback(GLFWwindow *window, int iconified) {
        ZoneScopedN("GLFW::glfw_window_iconify_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{iconified == GLFW_TRUE ? WindowEventKind::Minimized : WindowEventKind::Restored};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    /// GLFW delivers a finished drop as one callback with every path: that becomes the same Begin / File* / Complete
    /// sequence SDL3 emits. GLFW reports files only (no text drops) and no pointer position while dragging.
    void glfw_drop_callback(GLFWwindow *window, int count, const char **paths) {
        ZoneScopedN("GLFW::glfw_drop_callback");
        GLFWWindow *target = window_from_glfw(window);
        if (target == nullptr || count <= 0 || paths == nullptr) [[unlikely]] {
            return;
        }
        f64 x = 0.0;
        f64 y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        const WindowPosition position{static_cast<i32>(x), static_cast<i32>(y)};
        const u64 now = steady_now_ns();
        const auto push = [&](WindowEventKind kind) {
            WindowEvent event{kind};
            event.timestamp_ns = now;
            event.position = position;
            target->events_.push_back(event);
        };
        push(WindowEventKind::DropBegin);
        for (int i = 0; i < count; ++i) {
            if (paths[i] != nullptr) {
                push_dropped_item(target->id(), DroppedItem{false, paths[i]});
                push(WindowEventKind::DropFile);
            }
        }
        push(WindowEventKind::DropComplete);
    }

    /// The OS asks for the window's contents to be redrawn (uncovered, restored, ...).
    void glfw_window_refresh_callback(GLFWwindow *window) {
        ZoneScopedN("GLFW::glfw_window_refresh_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{WindowEventKind::Exposed};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    /// A monitor was connected or disconnected. GLFW reports this process-wide, so every window hears about it.
    void glfw_monitor_callback(GLFWmonitor *, int event) {
        ZoneScopedN("GLFW::glfw_monitor_callback");
        const WindowEventKind kind = event == GLFW_CONNECTED ? WindowEventKind::DisplayAdded : WindowEventKind::DisplayRemoved;
        const u64 now = steady_now_ns();
        for (GLFWWindow *target : glfw_live_windows()) {
            WindowEvent display_event{kind};
            display_event.timestamp_ns = now;
            target->events_.push_back(display_event);
        }
    }

    /// Handles the GLFW content-scale callback: caches the new scale and queues a ContentScaleChanged event.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x_scale Horizontal content scale.
    /// @param y_scale Vertical content scale (unused; matches x_scale on every platform).
    void glfw_window_content_scale_callback(GLFWwindow *window, float x_scale, float y_scale) {
        ZoneScopedN("GLFW::glfw_window_content_scale_callback");
        (void)y_scale;
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            const f32 scale = x_scale > 0.0f ? x_scale : 1.0f;
            target->content_scale_.store(scale, std::memory_order_relaxed);
            WindowEvent event{WindowEventKind::ContentScaleChanged};
            event.timestamp_ns = steady_now_ns();
            event.content_scale = scale;
            target->events_.push_back(event);
        }
    }

    /// Handles the GLFW framebuffer size callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Handles the GLFW window focus callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param focused `focused` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_window_focus_callback(GLFWwindow *window, int focused) {
        ZoneScopedN("GLFW::glfw_window_focus_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{focused == GLFW_TRUE ? WindowEventKind::FocusGained
                                                    : WindowEventKind::FocusLost};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    /// Handles the GLFW cursor enter callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param entered `entered` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_cursor_enter_callback(GLFWwindow *window, int entered) {
        ZoneScopedN("GLFW::glfw_cursor_enter_callback");
        if (GLFWWindow *target = window_from_glfw(window)) [[likely]] {
            WindowEvent event{entered == GLFW_TRUE ? WindowEventKind::MouseEntered
                                                    : WindowEventKind::MouseLeft};
            event.timestamp_ns = steady_now_ns();
            target->events_.push_back(event);
        }
    }

    /// Handles the GLFW key callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param key Key used to identify the requested entry.
    /// @param scancode `scancode` value used by the operation.
    /// @param action `action` value used by the operation.
    /// @param mods `mods` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Handles the GLFW char callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param codepoint `codepoint` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


    /// Performs the GLFW native preedit trampoline operation for `GLFW` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param cursor_pos `cursor_pos` value used by the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void glfw_native_preedit_trampoline(const char *text, int cursor_pos, void *user_data) {
        ZoneScopedN("GLFW::glfw_native_preedit_trampoline");
        auto *target = static_cast<GLFWWindow *>(user_data);
        if (!target) [[unlikely]] {
            return;
        }
        WindowEvent event{WindowEventKind::TextEditing};
        event.timestamp_ns = steady_now_ns();
        if (text) [[likely]] {
            copy_utf8_truncated(event.editing.utf8, sizeof(event.editing.utf8), text);
        }
        event.editing.cursor = cursor_pos;
        event.editing.selection_length = 0;
        target->events_.push_back(event);
    }

    /// Handles the GLFW cursor pos callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Handles the GLFW mouse button callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param button `button` value used by the operation.
    /// @param action `action` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            // Custom title bar on a platform with no hit-test message (X11, macOS): a press on a drag region or edge
            // asks the window system to start the move/resize, the way SDL's hit test would have.
            if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT && target->hit_test_.enabled &&
                Detail::hit_test_is_press_driven(window)) {
                int width = 0;
                int height = 0;
                glfwGetWindowSize(window, &width, &height);
                const bool can_resize = glfwGetWindowAttrib(window, GLFW_RESIZABLE) == GLFW_TRUE &&
                                        glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != GLFW_TRUE;
                const HitTestResult hit = classify_hit(
                    target->hit_test_, WindowExtent{static_cast<u32>(std::max(width, 0)), static_cast<u32>(std::max(height, 0))},
                    WindowPosition{static_cast<i32>(x), static_cast<i32>(y)}, can_resize);
                if (hit != HitTestResult::Normal) {
                    (void)Detail::begin_interactive_drag(window, hit);
                }
            }
        }
    }

    /// Handles the GLFW scroll callback callback and updates the associated platform state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Performs the glfwwindow operation for `GLFW` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    /// @param window Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
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
            content_scale_.store(glfw_content_scale(window_), std::memory_order_relaxed);
        }
    }

    /// Destroys the `GLFW` and releases resources owned by it.
    ///
    /// @note This function does not throw exceptions.
    GLFWWindow::~GLFWWindow() noexcept {
        ZoneScopedN("GLFWWindow::~GLFWWindow");
        auto lock = glfw_window_mutex().lock();

        clear_animated_cursor_locked();
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
        Detail::remove_ime_composition_hook(window_);
        Detail::remove_hit_test_hook(window_);
        std::erase(glfw_live_windows(), this);
        active_blur_effect_.reset();
        active_corner_effect_.reset();
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

    /// Constructs the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param key Key used to identify the requested entry.
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`, `WindowErrorCode::Unsupported`, `WindowErrorCode::BackendUnavailable`, `WindowErrorCode::CreationFailed`, `WindowErrorCode::OutOfMemory`.
    /// @note Allocation failure is converted to the implementation's out-of-memory error/status rather than escaping as `std::bad_alloc`.
    /// @note This function does not throw exceptions.
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
#if defined(_WIN32)
        // See GLFW::Detail::apply_composition_window_style's own doc comment for why this style has
        // to be set before the window is ever shown, and why that forces window creation itself
        // hidden here whenever the caller actually asked for a visible window — mirrors
        // SDL3Impl.cpp's identical force_hidden_for_composition_style handling.
        const bool needs_composition_style = wants_composition_window_style(config);
        const bool force_hidden_for_composition_style = needs_composition_style && config.visible;
        if (force_hidden_for_composition_style) {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        }
#endif
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

#if defined(_WIN32)
        // Dark title bar is unconditional (every GLFW window on Windows gets SDL3's default
        // appearance instead of GLFW's own white title bar) — see
        // GLFW::Detail::apply_windows_appearance's own doc comment. The WM_ERASEBKGND black-fill is
        // NOT applied to composition-style windows: GetDC/FillRect there would force GDI to allocate
        // the very redirection surface WS_EX_NOREDIRECTIONBITMAP (applied just below) exists to
        // suppress, permanently blocking a transparent DirectComposition swapchain behind an opaque
        // black surface — see apply_windows_appearance's own doc comment on `paint_erase_background`.
        // Applied before any show, same reasoning as apply_composition_window_style.
        GLFW::Detail::apply_windows_appearance(window, /*paint_erase_background=*/!needs_composition_style);
        if (needs_composition_style) {
            GLFW::Detail::apply_composition_window_style(window);
        }
        if (force_hidden_for_composition_style) {
            glfwShowWindow(window);
        }
#endif

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

        if (config.mode == WindowMode::Windowed) {
            const WindowExtent window_extent = glfw_window_extent_from_physical(window, config.extent);
            if (window_extent != config.extent) {
                glfwSetWindowSize(window, static_cast<int>(window_extent.x), static_cast<int>(window_extent.y));
            }
        }

        try {
            auto wrapper = unique_ptr<GLFWWindow>(new GLFWWindow(key, window));


            wrapper->fullscreen_mode_ = config.mode;
            glfwSetWindowUserPointer(window, wrapper.get());
            glfwSetWindowCloseCallback(window, glfw_close_callback);
            glfwSetWindowPosCallback(window, glfw_window_pos_callback);
            glfwSetWindowSizeCallback(window, glfw_window_size_callback);
            glfwSetFramebufferSizeCallback(window, glfw_framebuffer_size_callback);
            glfwSetWindowContentScaleCallback(window, glfw_window_content_scale_callback);
            glfwSetWindowFocusCallback(window, glfw_window_focus_callback);
            glfwSetCursorEnterCallback(window, glfw_cursor_enter_callback);
            glfwSetKeyCallback(window, glfw_key_callback);
            glfwSetCharCallback(window, glfw_char_callback);
            glfwSetCursorPosCallback(window, glfw_cursor_pos_callback);
            glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
            glfwSetScrollCallback(window, glfw_scroll_callback);
            glfwSetWindowMaximizeCallback(window, glfw_window_maximize_callback);
            glfwSetWindowIconifyCallback(window, glfw_window_iconify_callback);
            glfwSetDropCallback(window, glfw_drop_callback);
            glfwSetWindowRefreshCallback(window, glfw_window_refresh_callback);
            glfwSetMonitorCallback(glfw_monitor_callback); // process-wide; setting it again is harmless
            glfw_live_windows().push_back(wrapper.get());


            (void)Detail::install_ime_composition_hook(window, &glfw_native_preedit_trampoline, wrapper.get());
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

    /// Returns the current or globally available backend kind value.
    ///
    /// @return Returns the current backend kind value.
    /// @note This function does not throw exceptions.
    WindowBackendKind GLFWWindow::backend_kind() const noexcept {
        ZoneScopedN("GLFWWindow::backend_kind");
        return WindowBackendKind::GLFW;
    }

    /// Returns the runtime or backend type represented by `GLFW`.
    ///
    /// @return Returns the current type value.
    /// @note This function does not throw exceptions.
    WindowingSystem GLFWWindow::type() const noexcept {
        ZoneScopedN("GLFWWindow::type");
        return WindowingSystem::GLFW;
    }

    /// Returns the native backend handle associated with this `GLFW`.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the native window handle associated with this `GLFW`.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<NativeWindowHandle, WindowError> GLFWWindow::native_window_handle() const noexcept {
        ZoneScopedN("GLFWWindow::native_window_handle");
        auto lock = glfw_window_mutex().lock();
        return native_window_handle_locked();
    }

    /// Returns the current or globally available native window handle locked value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Pumps events using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::pump_events() noexcept {
        ZoneScopedN("GLFWWindow::pump_events");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "pump_events"); !live) [[unlikely]] {
            return live;
        }


        Foundation::log_trace("GLFW poll events begin: wrapper={} native_ptr={} "
                             "close_requested_before={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested_locked());
        glfwPollEvents();
        poll_system_theme_locked();
        advance_animated_cursor_locked();
        Desktop::pump_events(); // tray clicks: this program is not pumping SDL itself
        Foundation::log_trace("GLFW poll events complete: wrapper={} native_ptr={} "
                             "close_requested_after={}",
                             static_cast<void *>(this),
                             static_cast<void *>(window_),
                             close_requested_locked());
        return {};
    }

    /// Polls event for available work or state changes.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
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

    /// Closes requested using the supplied arguments and current state.
    ///
    /// @return Returns the current close requested value.
    /// @note This function does not throw exceptions.
    bool GLFWWindow::close_requested() const noexcept {
        ZoneScopedN("GLFWWindow::close_requested");
        auto lock = glfw_window_mutex().lock();
        return close_requested_locked();
    }

    /// Closes requested locked using the supplied arguments and current state.
    ///
    /// @return Returns the current close requested locked value.
    /// @note This function does not throw exceptions.
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

    /// Requests close using the supplied arguments and current state.
    ///
    /// @return Returns the current request close value.
    /// @note This function does not throw exceptions.
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

    /// Changes the logical size to the requested value, creating or removing elements as needed.
    ///
    /// @return Returns the current resized value.
    /// @note This function does not throw exceptions.
    bool GLFWWindow::resized() const noexcept {
        ZoneScopedN("GLFWWindow::resized");
        auto lock = glfw_window_mutex().lock();
        return pending_resize_.has_value();
    }

    /// Returns the current or globally available consume resize value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    optional<WindowResize> GLFWWindow::consume_resize() noexcept {
        ZoneScopedN("GLFWWindow::consume_resize");
        auto lock = glfw_window_mutex().lock();
        optional<WindowResize> resize = pending_resize_;
        pending_resize_.reset();
        return resize;
    }

    /// Returns the current or globally available show value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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
        {
            // GLFW has no visibility callback, so the change this call makes is reported here.
            WindowEvent shown{WindowEventKind::Shown};
            shown.timestamp_ns = steady_now_ns();
            events_.push_back(shown);
        }
        // Wayland destroys a hidden window's surface, taking its blur object with it; the recreated one needs the effect
        // again. A no-op elsewhere (the effect is simply applied twice).
        if (active_blur_effect_) {
            if (const auto handle = native_window_handle_locked()) {
                fill_effect_geometry_locked(*active_blur_effect_);
                (void)enable_native_window_effect(*handle, *active_blur_effect_);
            }
        }
        return glfw_success();
    }

    /// Returns the current or globally available hide value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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
        {
            WindowEvent hidden{WindowEventKind::Hidden};
            hidden.timestamp_ns = steady_now_ns();
            events_.push_back(hidden);
        }
        return glfw_success();
    }

    /// Returns the current or globally available focus value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available raise value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available maximize value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available minimize value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available restore value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the title for this `GLFW`.
    ///
    /// @param title `title` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available position value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the position for this `GLFW`.
    ///
    /// @param position `position` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available global cursor position value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
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


        f64 x = 0.0;
        f64 y = 0.0;
        glfwGetCursorPos(window_, &x, &y);
        int window_x = 0;
        int window_y = 0;
        glfwGetWindowPos(window_, &window_x, &window_y);
        return WindowPosition{window_x + static_cast<i32>(x), window_y + static_cast<i32>(y)};
    }

    /// Returns the size for this `GLFW`.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<WindowExtent, WindowError> GLFWWindow::size() const noexcept {
        ZoneScopedN("GLFWWindow::size");
        auto lock = glfw_window_mutex().lock();
        return size_locked();
    }

    /// Returns the current or globally available size locked value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the size for this `GLFW`.
    ///
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    /// @note This function does not throw exceptions.
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
        const WindowExtent window_extent = glfw_window_extent_from_physical(window_, extent);
        glfwSetWindowSize(window_, static_cast<int>(window_extent.x), static_cast<int>(window_extent.y));
        return glfw_success();
    }

    /// Returns the framebuffer size for this `GLFW`.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the cached content scale.
    ///
    /// @return The window's content scale.
    /// @note This function does not throw exceptions.
    f32 GLFWWindow::content_scale() const noexcept { return content_scale_.load(std::memory_order_relaxed); }

    /// Returns the current refresh rate, in Hz, of whichever monitor this window is presently on.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<f32, WindowError> GLFWWindow::refresh_rate_hz() const noexcept {
        ZoneScopedN("GLFWWindow::refresh_rate_hz");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "refresh_rate_hz"); !live) [[unlikely]] {
            return unexpected(live.error());
        }

        // Fullscreen: GLFW already knows the exact monitor. Windowed: GLFW has no "monitor this window is on"
        // query, so the nearest equivalent is the monitor whose bounds overlap the window's the most -- the
        // same heuristic every other windowing API's own "get monitor for window" boils down to internally.
        GLFWmonitor *monitor = glfwGetWindowMonitor(window_);
        if (monitor == nullptr) {
            int win_x = 0;
            int win_y = 0;
            int win_w = 0;
            int win_h = 0;
            glfwGetWindowPos(window_, &win_x, &win_y);
            glfwGetWindowSize(window_, &win_w, &win_h);

            int monitor_count = 0;
            GLFWmonitor **monitors = glfwGetMonitors(&monitor_count);
            i64 best_overlap_area = -1;
            for (int i = 0; i < monitor_count; ++i) {
                int mon_x = 0;
                int mon_y = 0;
                glfwGetMonitorPos(monitors[i], &mon_x, &mon_y);
                const GLFWvidmode *mode = glfwGetVideoMode(monitors[i]);
                if (mode == nullptr) {
                    continue;
                }
                const int overlap_x =
                    std::max(0, std::min(win_x + win_w, mon_x + mode->width) - std::max(win_x, mon_x));
                const int overlap_y =
                    std::max(0, std::min(win_y + win_h, mon_y + mode->height) - std::max(win_y, mon_y));
                const i64 overlap_area = static_cast<i64>(overlap_x) * static_cast<i64>(overlap_y);
                if (overlap_area > best_overlap_area) {
                    best_overlap_area = overlap_area;
                    monitor = monitors[i];
                }
            }
            if (monitor == nullptr) {
                // No monitor at all (extremely unusual) or the window sits entirely off every one of
                // them (a moment mid-drag): fall back to whichever GLFW considers primary.
                monitor = glfwGetPrimaryMonitor();
            }
        }
        if (monitor == nullptr) {
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW reports no monitors."});
        }

        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode == nullptr || mode->refreshRate <= 0) {
            return unexpected(WindowError{WindowErrorCode::OperationFailed,
                                          "GLFW reported no usable refresh rate for this monitor."});
        }
        return static_cast<f32>(mode->refreshRate);
    }

    /// Sets the minimum size for this `GLFW`.
    ///
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    /// @note This function does not throw exceptions.
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
        const WindowExtent window_extent = glfw_window_extent_from_physical(window_, extent);
        glfwSetWindowSizeLimits(window_, static_cast<int>(window_extent.x), static_cast<int>(window_extent.y), GLFW_DONT_CARE, GLFW_DONT_CARE);
        return glfw_success();
    }

    /// Sets the maximum size for this `GLFW`.
    ///
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    /// @note This function does not throw exceptions.
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
        const WindowExtent window_extent = glfw_window_extent_from_physical(window_, extent);
        glfwSetWindowSizeLimits(window_, GLFW_DONT_CARE, GLFW_DONT_CARE, static_cast<int>(window_extent.x), static_cast<int>(window_extent.y));
        return glfw_success();
    }

    /// Sets the resizable for this `GLFW`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the decorated for this `GLFW`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the fullscreen for this `GLFW`.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_fullscreen(WindowMode mode) noexcept {
        ZoneScopedN("GLFWWindow::set_fullscreen");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_fullscreen"); !live) [[unlikely]] {
            return live;
        }
        if (mode == WindowMode::Windowed) [[likely]] {


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
            if (fullscreen_mode_ != WindowMode::Windowed) {
                WindowEvent left{WindowEventKind::LeftFullscreen};
                left.timestamp_ns = steady_now_ns();
                events_.push_back(left);
            }
            fullscreen_mode_ = mode;
            return glfw_success();
        }

        GLFWmonitor *monitor = monitor_for_target(window_, fullscreen_target_);
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
        // Only an explicit mode makes the fullscreen exclusive; without one both kinds stay at the desktop mode.
        int fullscreen_width = mode_info->width;
        int fullscreen_height = mode_info->height;
        int fullscreen_refresh = mode_info->refreshRate;
        if (mode == WindowMode::ExclusiveFullscreen && fullscreen_target_.mode.has_value()) {
            fullscreen_width = static_cast<int>(fullscreen_target_.mode->width);
            fullscreen_height = static_cast<int>(fullscreen_target_.mode->height);
            fullscreen_refresh = static_cast<int>(fullscreen_target_.mode->refresh_rate_hz + 0.5f);
        }
        const bool was_fullscreen = fullscreen_mode_ != WindowMode::Windowed;
        glfwSetWindowMonitor(window_, monitor, 0, 0, fullscreen_width, fullscreen_height, fullscreen_refresh);

        fullscreen_mode_ = mode;
        if (!was_fullscreen) {
            WindowEvent entered{WindowEventKind::EnteredFullscreen};
            entered.timestamp_ns = steady_now_ns();
            events_.push_back(entered);
        }
        return glfw_success();
    }

    /// Returns the current or globally available fullscreen mode value.
    ///
    /// @return Returns the current fullscreen mode value.
    /// @note This function does not throw exceptions.
    WindowMode GLFWWindow::fullscreen_mode() const noexcept {
        return fullscreen_mode_;
    }

    /// Sets the opacity for this `GLFW`.
    ///
    /// @param opacity `opacity` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available opacity value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the cursor visible for this `GLFW`.
    ///
    /// @param visible `visible` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the cursor icon for this `GLFW`.
    ///
    /// @param icon `icon` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_cursor_icon(CursorIcon icon) noexcept {
        ZoneScopedN("GLFWWindow::set_cursor_icon");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_icon"); !live) [[unlikely]] {
            return live;
        }


        if (current_cursor_icon_.has_value() && *current_cursor_icon_ == icon) {
            return glfw_success();
        }
        const int standard_shape = to_glfw_standard_cursor(icon);
        if (standard_shape < 0) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW has no busy (wait/progress) cursor; use set_custom_cursor."});
        }
        GLFWcursor *cursor = glfwCreateStandardCursor(standard_shape);
        if (cursor == nullptr) [[unlikely]] {
            Foundation::log_error(
                "GLFW create standard cursor failed: wrapper={} native_ptr={} icon={}",
                static_cast<void *>(this), static_cast<void *>(window_), static_cast<i32>(icon));
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW create standard cursor failed."});
        }
        clear_animated_cursor_locked();
        glfwSetCursor(window_, cursor);
        if (current_cursor_ != nullptr) {
            glfwDestroyCursor(current_cursor_);
        }
        current_cursor_ = cursor;
        current_cursor_icon_ = icon;
        custom_cursor_active_ = false;
        Foundation::log_trace(
            "GLFW set cursor icon: wrapper={} native_ptr={} icon={}",
            static_cast<void *>(this), static_cast<void *>(window_), static_cast<i32>(icon));
        return glfw_success();
    }

    /// Sets the cursor grabbed for this `GLFW`.
    ///
    /// @param grabbed `grabbed` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the relative mouse mode for this `GLFW`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Sets the mouse locked for this `GLFW`.
    ///
    /// @param locked `locked` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available mouse locked value.
    ///
    /// @return Returns the current mouse locked value.
    /// @note This function does not throw exceptions.
    bool GLFWWindow::mouse_locked() const noexcept {
        ZoneScopedN("GLFWWindow::mouse_locked");
        auto lock = glfw_window_mutex().lock();
        return mouse_locked_;
    }

    /// Enables window effect using the supplied arguments and current state.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
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


        auto handle = native_window_handle_locked();
        if (!handle) [[unlikely]] {
            return WindowEffectResult::failed("GLFW native window handle is unavailable.");
        }

        // Platform code turns the effect options (rounded corners, regions) into pixel regions and needs the size.
        fill_effect_geometry_locked(effect);
        WindowEffectResult result = enable_native_window_effect(*handle, effect);

        // Remember the effects whose shape follows the window, as SDL3 does, so a resize rebuilds them.
        if (result.succeeded()) {
            if (effect.kind == WindowEffectKind::Blur) {
                if (effect.enabled) {
                    active_blur_effect_ = effect;
                } else {
                    active_blur_effect_.reset();
                }
            } else if (effect.kind == WindowEffectKind::CornerRadius) {
                if (effect.enabled && effect.options.outline_follows_window_size()) {
                    active_corner_effect_ = effect;
                } else {
                    active_corner_effect_.reset();
                }
            }
        }
        return result;
    }

    void GLFWWindow::fill_effect_geometry_locked(WindowEffect &effect) const noexcept {
        int width = 0;
        int height = 0;
        glfwGetWindowSize(window_, &width, &height);
        effect.options.window_width = static_cast<u32>(width > 0 ? width : 0);
        effect.options.window_height = static_cast<u32>(height > 0 ? height : 0);
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);
        effect.options.pixel_scale = width > 0 ? static_cast<f32>(framebuffer_width) / static_cast<f32>(width) : 1.0f;
    }

    void GLFWWindow::reapply_size_dependent_effects_locked() noexcept {
        if (window_ == nullptr || (!active_blur_effect_ && !active_corner_effect_)) {
            return;
        }
        const auto handle = native_window_handle_locked();
        if (!handle) {
            return;
        }
        if (active_blur_effect_ && active_blur_effect_->options.region_follows_window_size()) {
            fill_effect_geometry_locked(*active_blur_effect_);
            (void)enable_native_window_effect(*handle, *active_blur_effect_);
        }
        if (active_corner_effect_) {
            fill_effect_geometry_locked(*active_corner_effect_);
            (void)enable_native_window_effect(*handle, *active_corner_effect_);
        }
    }

    /// Sets the effect for this `GLFW`.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_effect(WindowEffect effect) noexcept {
        ZoneScopedN("GLFWWindow::set_effect");
        return window_result_from_effect_result(enable_window_effect(effect));
    }

    /// Sets the blur enabled for this `GLFW`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_blur_enabled(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_blur_enabled");
        return set_effect(WindowEffect::blur(enabled));
    }

    /// Sets the transparent for this `GLFW`.
    ///
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_transparent(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_transparent");
        return set_effect(WindowEffect::transparent(enabled));
    }

    /// Returns the current or globally available required vulkan instance extensions value.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::OutOfMemory`.
    /// @note This function does not throw exceptions.
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

    /// Creates a vulkan surface from the supplied parameters.
    ///
    /// @param instance Instance used or affected by the operation.
    /// @param allocation_callbacks Callable invoked by the operation.
    /// @param surface_out Surface used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`, `WindowErrorCode::CreationFailed`.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available clipboard text value.
    ///
    /// @return Returns the current clipboard text value.
    /// @note This function does not throw exceptions.
    std::string GLFWWindow::clipboard_text() const noexcept {
        ZoneScopedN("GLFWWindow::clipboard_text");
        const char *text = glfwGetClipboardString(window_);
        return text ? std::string(text) : std::string();
    }

    /// Sets the clipboard text for this `GLFW`.
    ///
    /// @param text Text consumed by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_clipboard_text(std::string_view text) noexcept {
        ZoneScopedN("GLFWWindow::set_clipboard_text");


        const std::string owned(text);
        glfwSetClipboardString(window_, owned.c_str());
        return {};
    }

    /// Sets the text input area for this `GLFW`.
    ///
    /// @param area `area` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::set_text_input_area(TextInputArea area) noexcept {
        ZoneScopedN("GLFWWindow::set_text_input_area");
        // TextInputArea is in physical pixels; the native IME hooks take window coordinates.
        const glm::vec2 density = glfw_pixel_density(window_);
        Detail::set_ime_composition_exclude_rect(window_, static_cast<int>(area.x / density.x), static_cast<int>(area.y / density.y),
                                                 static_cast<int>(area.width / density.x), static_cast<int>(area.height / density.y));
        return {};
    }

    /// Starts text input using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::start_text_input() noexcept {
        ZoneScopedN("GLFWWindow::start_text_input");
        Detail::set_ime_enabled(window_, true);
        return {};
    }

    /// Stops text input using the supplied arguments and current state.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    expected<void, WindowError> GLFWWindow::stop_text_input() noexcept {
        ZoneScopedN("GLFWWindow::stop_text_input");
        Detail::set_ime_enabled(window_, false);
        return {};
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Window chrome and desktop integration
    // ---------------------------------------------------------------------------------------------------------------

    expected<void, WindowError> GLFWWindow::set_hit_test_regions(HitTestRegions regions) noexcept {
        ZoneScopedN("GLFWWindow::set_hit_test_regions");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_hit_test_regions"); !live) [[unlikely]] {
            return live;
        }
        const bool enabled = regions.enabled;
        hit_test_ = std::move(regions);
        if (!enabled) {
            Detail::remove_hit_test_hook(window_);
            return {};
        }
        if (!Detail::install_hit_test_hook(window_, &hit_test_)) {
            hit_test_.enabled = false;
            return unexpected(WindowError{WindowErrorCode::Unsupported,
                                          "This window system gives GLFW no way to move or resize an undecorated window "
                                          "(Wayland exposes the toplevel to GLFW only internally)."});
        }
        return {};
    }

    expected<void, WindowError> GLFWWindow::show_system_menu(WindowPosition position) noexcept {
        ZoneScopedN("GLFWWindow::show_system_menu");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "show_system_menu"); !live) [[unlikely]] {
            return live;
        }
        if (!Detail::show_native_system_menu(window_, position.x, position.y)) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "The window system has no window menu."});
        }
        return {};
    }

    bool GLFWWindow::maximized() const noexcept {
        auto lock = glfw_window_mutex().lock();
        return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    }

    bool GLFWWindow::minimized() const noexcept {
        auto lock = glfw_window_mutex().lock();
        return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE;
    }

    expected<void, WindowError> GLFWWindow::set_icon(u32 width, u32 height, std::span<const std::byte> rgba) noexcept {
        ZoneScopedN("GLFWWindow::set_icon");
        if (width == 0 || height == 0 || rgba.size() < static_cast<usize>(width) * height * 4) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Window icon needs width * height * 4 bytes of RGBA8."});
        }
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_icon"); !live) [[unlikely]] {
            return live;
        }
        (void)glfwGetError(nullptr); // drop any stale error so the check below is about this call
        GLFWimage image{static_cast<int>(width), static_cast<int>(height),
                        const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(rgba.data()))};
        glfwSetWindowIcon(window_, 1, &image);
        const char *description = nullptr;
        if (glfwGetError(&description) != GLFW_NO_ERROR) {
            // GLFW cannot set icons on Wayland (the compositor uses the desktop file matching the app id) or macOS.
            return unexpected(WindowError{WindowErrorCode::Unsupported,
                                          description != nullptr && description[0] != '\0' ? description : "The window system rejected the window icon."});
        }
        return {};
    }

    expected<void, WindowError> GLFWWindow::request_attention(AttentionRequest request) noexcept {
        ZoneScopedN("GLFWWindow::request_attention");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "request_attention"); !live) [[unlikely]] {
            return live;
        }
        if (request == AttentionRequest::Cancel) {
            // GLFW can ask for attention but never withdraw the request; focusing the window is what clears it.
            return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW cannot cancel an attention request."});
        }
        // GLFW's request is "until the window gets focus" on every platform; there is no brief variant.
        glfwRequestWindowAttention(window_);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_taskbar_progress(TaskbarProgress state, f32 value) noexcept {
        ZoneScopedN("GLFWWindow::set_taskbar_progress");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_taskbar_progress"); !live) [[unlikely]] {
            return live;
        }
        if (!Detail::set_native_taskbar_progress(window_, state, value)) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "The window system has no taskbar progress."});
        }
        return {};
    }

    SystemTheme GLFWWindow::system_theme() const noexcept { return Detail::native_system_theme(); }

    void GLFWWindow::poll_system_theme_locked() noexcept {
        // GLFW has no theme-change event, so compare against the OS preference about every two seconds.
        constexpr u64 poll_interval_ns = 2'000'000'000ull;
        const u64 now = steady_now_ns();
        if (last_theme_poll_ns_ != 0 && now - last_theme_poll_ns_ < poll_interval_ns) {
            return;
        }
        last_theme_poll_ns_ = now;
        const SystemTheme current = Detail::native_system_theme();
        if (current == last_system_theme_) {
            return;
        }
        const bool first_reading = last_system_theme_ == SystemTheme::Unknown;
        last_system_theme_ = current;
        if (!first_reading && current != SystemTheme::Unknown) {
            WindowEvent event{WindowEventKind::SystemThemeChanged};
            event.timestamp_ns = now;
            events_.push_back(event);
        }
    }

    std::vector<DisplayInfo> GLFWWindow::displays() const noexcept {
        std::vector<DisplayInfo> out;
        auto lock = glfw_window_mutex().lock();
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        if (monitors == nullptr) {
            return out;
        }
        GLFWmonitor *primary = glfwGetPrimaryMonitor();
        for (int i = 0; i < count; ++i) {
            GLFWmonitor *monitor = monitors[i];
            DisplayInfo info;
            info.id = static_cast<u32>(i + 1);
            if (const char *name = glfwGetMonitorName(monitor)) {
                info.name = name;
            }
            int x = 0;
            int y = 0;
            glfwGetMonitorPos(monitor, &x, &y);
            if (const GLFWvidmode *mode = glfwGetVideoMode(monitor)) {
                info.bounds = WindowRect{x, y, mode->width, mode->height};
                info.refresh_rate_hz = static_cast<f32>(mode->refreshRate);
            }
            int work_x = 0;
            int work_y = 0;
            int work_width = 0;
            int work_height = 0;
            glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_width, &work_height);
            info.usable_bounds = WindowRect{work_x, work_y, work_width, work_height};
            float scale_x = 1.0f;
            float scale_y = 1.0f;
            glfwGetMonitorContentScale(monitor, &scale_x, &scale_y);
            info.content_scale = scale_x;
            if (const GLFWvidmode *mode = glfwGetVideoMode(monitor)) {
                info.current_mode = DisplayMode{static_cast<u32>(mode->width), static_cast<u32>(mode->height),
                                                static_cast<f32>(mode->refreshRate), scale_x};
            }
            // GLFW has no rotation query; width versus height is the orientation a desktop display can show.
            if (info.current_mode.width != 0) {
                info.orientation = info.current_mode.width >= info.current_mode.height ? DisplayOrientation::Landscape : DisplayOrientation::Portrait;
            }
            int mode_count = 0;
            if (const GLFWvidmode *modes = glfwGetVideoModes(monitor, &mode_count)) {
                for (int m = 0; m < mode_count; ++m) {
                    info.modes.push_back(DisplayMode{static_cast<u32>(modes[m].width), static_cast<u32>(modes[m].height),
                                                     static_cast<f32>(modes[m].refreshRate), scale_x});
                }
            }
            int physical_width = 0;
            int physical_height = 0;
            glfwGetMonitorPhysicalSize(monitor, &physical_width, &physical_height);
            info.physical_width_mm = static_cast<u32>(std::max(physical_width, 0));
            info.physical_height_mm = static_cast<u32>(std::max(physical_height, 0));
            info.hdr_enabled = false; // GLFW has no display HDR query
            info.primary = monitor == primary;
            out.push_back(std::move(info));
        }
        return out;
    }

    optional<WindowHdrProperties> GLFWWindow::hdr_properties() const noexcept {
        ZoneScopedN("GLFWWindow::hdr_properties");
#if defined(__linux__)
        // GLFW has no HDR query of its own; a Wayland compositor's colour-management protocol still reports the
        // reference white, the same source the SDL3 provider reads.
        auto lock = glfw_window_mutex().lock();
        if (window_ != nullptr) {
            if (const auto handle = native_window_handle_locked(); handle && handle->system == NativeWindowSystem::Wayland) {
                if (const auto nits = ::SFT::WindowManager::Detail::query_wayland_surface_reference_white(*handle);
                    nits && *nits > 0.0f) {
                    WindowHdrProperties properties;
                    properties.hdr_enabled = true;
                    properties.sdr_white_level = *nits / 80.0f;
                    return properties;
                }
            }
        }
#endif
        return nullopt; // Windows and macOS display HDR state is not reachable through GLFW
    }

    void GLFWWindow::set_live_resize_callback(std::function<void(WindowExtent)> callback) noexcept {
        ZoneScopedN("GLFWWindow::set_live_resize_callback");
        auto lock = glfw_window_mutex().lock();
        live_resize_callback_ = std::move(callback);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The union surface (WindowFeatures.hpp)
    // ---------------------------------------------------------------------------------------------------------------

    FeatureSupport GLFWWindow::feature_support(WindowFeature feature) const noexcept {
        // What the native layer can do depends on the session type, which is the native handle's `system`.
        NativeWindowSystem system = NativeWindowSystem::Unknown;
        {
            auto lock = glfw_window_mutex().lock();
            if (window_ != nullptr) {
                if (const auto handle = native_window_handle_locked()) {
                    system = handle->system;
                }
            }
        }
        const bool wayland = system == NativeWindowSystem::Wayland;
        const bool x11 = system == NativeWindowSystem::X11;
        const bool windows = system == NativeWindowSystem::Win32;
        const bool cocoa = system == NativeWindowSystem::Cocoa;
        switch (feature) {
            case WindowFeature::AlwaysOnTop:
            case WindowFeature::WarpCursor:
            case WindowFeature::CustomCursor:
            case WindowFeature::FrameExtents:
            case WindowFeature::FullscreenTarget:
            case WindowFeature::DisplayList:
            case WindowFeature::DisplayModes:
            case WindowFeature::FileDrop:
            case WindowFeature::ClipboardMimeTypes:
            case WindowFeature::AutoMinimizeOnFocusLoss:
                return FeatureSupport::yes();
            case WindowFeature::AspectRatio: return FeatureSupport::yes("one exact ratio, not a range");
            case WindowFeature::SafeArea: return FeatureSupport::yes("the whole client area");
            case WindowFeature::DisplayEvents: return FeatureSupport::yes("monitors connecting and disconnecting");
            case WindowFeature::WindowIcon:
                return wayland || cocoa ? FeatureSupport::no("Wayland and macOS take the icon from the desktop file / app bundle")
                                        : FeatureSupport::yes();
            case WindowFeature::RequestAttention: return FeatureSupport::yes("until the window is focused; it cannot be cancelled");
            case WindowFeature::HitTestRegions:
                return wayland ? FeatureSupport::no("GLFW does not expose the Wayland toplevel a move or resize request needs")
                               : cocoa ? FeatureSupport::yes("drag regions only; AppKit has no edge-resize request")
                                       : FeatureSupport::yes();
            case WindowFeature::SystemMenu:
                return windows || x11 ? FeatureSupport::yes() : FeatureSupport::no("the window system has no window menu to open");
            case WindowFeature::TaskbarProgress:
                return windows ? FeatureSupport::yes() : FeatureSupport::no("only Windows has a taskbar progress GLFW can reach");
            case WindowFeature::SystemTheme: return FeatureSupport::yes("read from the OS settings; Linux reads the GTK/KDE config files");
            case WindowFeature::LiveResizeCallback: return FeatureSupport::yes("Windows only: it is the one platform whose resize loop blocks polling");
            case WindowFeature::MouseConfineRect:
                return windows || x11 ? FeatureSupport::yes()
                                      : FeatureSupport::no("only Windows and X11 let a GLFW client confine the cursor to a rectangle");
            case WindowFeature::ParentWindow:
                return wayland ? FeatureSupport::no("GLFW does not expose the Wayland toplevel a parent must be set on") : FeatureSupport::yes();
            case WindowFeature::ModalWindow:
                return wayland ? FeatureSupport::no("GLFW does not expose the Wayland toplevel a parent must be set on")
                               : x11 ? FeatureSupport::yes("asked of the window manager")
                                     : FeatureSupport::yes("the window is owned by its parent; the parent is not disabled");
            case WindowFeature::Focusable:
                return x11 || windows ? Window::feature_support(feature) : FeatureSupport::no("not controllable here");
            case WindowFeature::TextInputOptions: return FeatureSupport::no("GLFW has no text input hints; the IME still works");
            case WindowFeature::ScreenKeyboard: return FeatureSupport::no("GLFW is desktop-only and has no on-screen keyboard");
            case WindowFeature::TypedClipboard:
                return windows || cocoa ? FeatureSupport::yes()
                                        : FeatureSupport::no("GLFW's clipboard is text only; X11 and Wayland selections need the event loop GLFW owns");
            case WindowFeature::TextDrop: return FeatureSupport::no("GLFW reports dropped files only");
            case WindowFeature::TouchInput: return FeatureSupport::no("GLFW has no touch events; the system synthesises mouse input from touches");
            case WindowFeature::PenInput: return FeatureSupport::no("GLFW has no pen events; the system synthesises mouse input from pens");
            case WindowFeature::AnimatedCursor: return FeatureSupport::yes("GLFW has no animated cursors; the frames are swapped on the poll clock");
            case WindowFeature::KeyboardQuery: return FeatureSupport::yes("key names exist for printable keys only");
            case WindowFeature::WarpCursorGlobal:
                return wayland ? FeatureSupport::no("Wayland does not let a client move the cursor") : FeatureSupport::yes();
            case WindowFeature::MouseCapture:
                return windows || x11 ? FeatureSupport::yes() : FeatureSupport::no("Wayland and AppKit already deliver drags to the pressed window");
            case WindowFeature::IccProfile: return FeatureSupport::no("GLFW cannot read a display's colour profile");
            case WindowFeature::WaitForEvents: return FeatureSupport::no("GLFW's blocking wait would hold the window lock; the manager polls");
            default: return Window::feature_support(feature);
        }
    }

    expected<void, WindowError> GLFWWindow::set_always_on_top(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_always_on_top");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_always_on_top"); !live) [[unlikely]] {
            return live;
        }
        glfwSetWindowAttrib(window_, GLFW_FLOATING, enabled ? GLFW_TRUE : GLFW_FALSE);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_aspect_ratio(f32 min_ratio, f32 max_ratio) noexcept {
        ZoneScopedN("GLFWWindow::set_aspect_ratio");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_aspect_ratio"); !live) [[unlikely]] {
            return live;
        }
        if (min_ratio < 0.0f || max_ratio < 0.0f || (max_ratio != 0.0f && min_ratio > max_ratio)) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Aspect ratios must satisfy 0 <= min <= max."});
        }
        if (min_ratio == 0.0f && max_ratio == 0.0f) {
            glfwSetWindowAspectRatio(window_, GLFW_DONT_CARE, GLFW_DONT_CARE);
            return glfw_success();
        }
        if (min_ratio != max_ratio) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW can hold one exact aspect ratio, not a range."});
        }
        // GLFW takes a fraction; ten-thousandths keep any practical ratio exact enough.
        glfwSetWindowAspectRatio(window_, static_cast<int>(min_ratio * 10000.0f + 0.5f), 10000);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_auto_minimize_on_focus_loss(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_auto_minimize_on_focus_loss");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_auto_minimize_on_focus_loss"); !live) [[unlikely]] {
            return live;
        }
        glfwSetWindowAttrib(window_, GLFW_AUTO_ICONIFY, enabled ? GLFW_TRUE : GLFW_FALSE);
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_mouse_confine_rect(std::optional<WindowRect> rect) noexcept {
        ZoneScopedN("GLFWWindow::set_mouse_confine_rect");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_mouse_confine_rect"); !live) [[unlikely]] {
            return live;
        }
        if (!Detail::set_native_cursor_confine(window_, rect ? &*rect : nullptr)) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "Only Windows and X11 let a GLFW client confine the cursor to a rectangle."});
        }
        return {};
    }

    expected<void, WindowError> GLFWWindow::set_cursor_position(WindowPosition position) noexcept {
        ZoneScopedN("GLFWWindow::set_cursor_position");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_position"); !live) [[unlikely]] {
            return live;
        }
        (void)glfwGetError(nullptr);
        glfwSetCursorPos(window_, static_cast<f64>(position.x), static_cast<f64>(position.y));
        const char *description = nullptr;
        if (glfwGetError(&description) != GLFW_NO_ERROR) {
            // Wayland refuses client-side cursor warping outside a pointer lock.
            return unexpected(WindowError{WindowErrorCode::Unsupported,
                                          description != nullptr && description[0] != '\0' ? description : "The window system refused to move the cursor."});
        }
        return {};
    }

    expected<void, WindowError> GLFWWindow::set_custom_cursor(const CursorImage &image) noexcept {
        ZoneScopedN("GLFWWindow::set_custom_cursor");
        if (image.width == 0 || image.height == 0 || image.rgba.size() < static_cast<usize>(image.width) * image.height * 4) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "A cursor image needs width * height * 4 bytes of RGBA8."});
        }
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_custom_cursor"); !live) [[unlikely]] {
            return live;
        }
        GLFWimage glfw_image{static_cast<int>(image.width), static_cast<int>(image.height),
                             const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(image.rgba.data()))};
        GLFWcursor *cursor = glfwCreateCursor(&glfw_image, image.hot_x, image.hot_y); // GLFW copies the pixels
        if (cursor == nullptr) {
            return unexpected(glfw_error(WindowErrorCode::OperationFailed, "GLFW could not create the cursor."));
        }
        clear_animated_cursor_locked();
        glfwSetCursor(window_, cursor);
        if (current_cursor_ != nullptr) {
            glfwDestroyCursor(current_cursor_);
        }
        current_cursor_ = cursor;
        current_cursor_icon_.reset(); // the next set_cursor_icon must really change the cursor
        custom_cursor_active_ = true;
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::clear_custom_cursor() noexcept {
        {
            auto lock = glfw_window_mutex().lock();
            if (!custom_cursor_active_) {
                return {};
            }
            current_cursor_icon_.reset();
            custom_cursor_active_ = false;
        }
        return set_cursor_icon(CursorIcon::Default);
    }

    void GLFWWindow::clear_animated_cursor_locked() noexcept {
        for (GLFWcursor *cursor : animated_cursors_) {
            glfwDestroyCursor(cursor);
        }
        animated_cursors_.clear();
        animated_durations_ms_.clear();
        animated_frame_ = 0;
    }

    void GLFWWindow::advance_animated_cursor_locked() noexcept {
        if (animated_cursors_.size() < 2 || window_ == nullptr) {
            return;
        }
        const u64 now = steady_now_ns();
        if (now < animated_next_swap_ns_) {
            return;
        }
        // Duration 0 means "stay on this frame forever", as in SDL's animated cursors.
        if (animated_durations_ms_[animated_frame_] == 0) {
            return;
        }
        animated_frame_ = (animated_frame_ + 1) % animated_cursors_.size();
        glfwSetCursor(window_, animated_cursors_[animated_frame_]);
        animated_next_swap_ns_ = now + static_cast<u64>(animated_durations_ms_[animated_frame_]) * 1'000'000ull;
    }

    expected<void, WindowError> GLFWWindow::set_animated_cursor(std::span<const CursorFrame> frames) noexcept {
        ZoneScopedN("GLFWWindow::set_animated_cursor");
        if (frames.empty()) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "An animated cursor needs at least one frame."});
        }
        const CursorImage &first = frames.front().image;
        for (const CursorFrame &frame : frames) {
            const CursorImage &image = frame.image;
            if (image.width == 0 || image.height == 0 || image.width != first.width || image.height != first.height ||
                image.rgba.size() < static_cast<usize>(image.width) * image.height * 4) {
                return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Every cursor frame needs the same size and width * height * 4 bytes of RGBA8."});
            }
        }
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_animated_cursor"); !live) [[unlikely]] {
            return live;
        }
        std::vector<GLFWcursor *> cursors;
        std::vector<u32> durations;
        for (const CursorFrame &frame : frames) {
            GLFWimage image{static_cast<int>(frame.image.width), static_cast<int>(frame.image.height),
                            const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(frame.image.rgba.data()))};
            GLFWcursor *cursor = glfwCreateCursor(&image, first.hot_x, first.hot_y); // the hot spot is shared by every frame
            if (cursor == nullptr) {
                const WindowError error = glfw_error(WindowErrorCode::OperationFailed, "GLFW could not create a cursor frame.");
                for (GLFWcursor *made : cursors) {
                    glfwDestroyCursor(made);
                }
                return unexpected(error);
            }
            cursors.push_back(cursor);
            durations.push_back(frame.duration_ms);
        }
        clear_animated_cursor_locked();
        if (current_cursor_ != nullptr) {
            glfwDestroyCursor(current_cursor_);
            current_cursor_ = nullptr;
        }
        animated_cursors_ = std::move(cursors);
        animated_durations_ms_ = std::move(durations);
        animated_frame_ = 0;
        glfwSetCursor(window_, animated_cursors_.front());
        animated_next_swap_ns_ = steady_now_ns() + static_cast<u64>(animated_durations_ms_.front()) * 1'000'000ull;
        current_cursor_icon_.reset();
        custom_cursor_active_ = true;
        return glfw_success();
    }

    expected<void, WindowError> GLFWWindow::set_global_cursor_position(WindowPosition position) noexcept {
        ZoneScopedN("GLFWWindow::set_global_cursor_position");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_global_cursor_position"); !live) [[unlikely]] {
            return live;
        }
        // GLFW warps within a window, so the desktop point is made relative to this window's origin.
        int window_x = 0;
        int window_y = 0;
        glfwGetWindowPos(window_, &window_x, &window_y);
        (void)glfwGetError(nullptr);
        glfwSetCursorPos(window_, static_cast<f64>(position.x - window_x), static_cast<f64>(position.y - window_y));
        const char *description = nullptr;
        if (glfwGetError(&description) != GLFW_NO_ERROR) {
            return unexpected(WindowError{WindowErrorCode::Unsupported,
                                          description != nullptr && description[0] != '\0' ? description : "The window system refused to move the cursor."});
        }
        return {};
    }

    expected<void, WindowError> GLFWWindow::set_mouse_capture(bool enabled) noexcept {
        ZoneScopedN("GLFWWindow::set_mouse_capture");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_mouse_capture"); !live) [[unlikely]] {
            return live;
        }
        if (!Detail::set_native_mouse_capture(window_, enabled)) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "Mouse capture is available on Windows and X11 only."});
        }
        return {};
    }

    namespace {
        /// The GLFW key whose layout key is `key`, or GLFW_KEY_UNKNOWN. Walks GLFW's key range through the same mapping
        /// the event path uses.
        [[nodiscard]] int glfw_key_for(KeyboardKey key) noexcept {
            for (int candidate = GLFW_KEY_SPACE; candidate <= GLFW_KEY_LAST; ++candidate) {
                if (keyboard_key(candidate) == key) {
                    return candidate;
                }
            }
            return GLFW_KEY_UNKNOWN;
        }
    } // namespace

    std::string GLFWWindow::key_name(KeyboardKey key) const noexcept {
        const int glfw_key = glfw_key_for(key);
        if (glfw_key == GLFW_KEY_UNKNOWN) {
            return {};
        }
        auto lock = glfw_window_mutex().lock();
        // Printable keys have a layout-dependent name; the rest (Enter, arrows, ...) have none in GLFW.
        const char *name = glfwGetKeyName(glfw_key, 0);
        return name != nullptr ? name : "";
    }

    bool GLFWWindow::key_down(KeyboardKey key) const noexcept {
        const int glfw_key = glfw_key_for(key);
        if (glfw_key == GLFW_KEY_UNKNOWN) {
            return false;
        }
        auto lock = glfw_window_mutex().lock();
        return window_ != nullptr && glfwGetKey(window_, glfw_key) == GLFW_PRESS;
    }

    expected<void, WindowError> GLFWWindow::set_display_gamma(std::optional<u32> display_id, f32 gamma) noexcept {
        ZoneScopedN("GLFWWindow::set_display_gamma");
        if (!(gamma > 0.0f)) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Gamma must be positive."});
        }
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_display_gamma"); !live) [[unlikely]] {
            return live;
        }
        GLFWmonitor *monitor = display_id.has_value() ? monitor_for_display_id(*display_id) : monitor_containing(window_);
        if (monitor == nullptr) {
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "That display does not exist."});
        }
        (void)glfwGetError(nullptr);
        glfwSetGamma(monitor, gamma);
        const char *description = nullptr;
        if (glfwGetError(&description) != GLFW_NO_ERROR) {
            return unexpected(WindowError{WindowErrorCode::Unsupported,
                                          description != nullptr && description[0] != '\0' ? description : "The window system cannot change the gamma ramp."});
        }
        return {};
    }

    std::optional<u32> GLFWWindow::current_display_id() const noexcept {
        auto lock = glfw_window_mutex().lock();
        if (window_ == nullptr) {
            return std::nullopt;
        }
        GLFWmonitor *current = monitor_containing(window_);
        int count = 0;
        GLFWmonitor **monitors = glfwGetMonitors(&count);
        for (int i = 0; monitors != nullptr && i < count; ++i) {
            if (monitors[i] == current) {
                return static_cast<u32>(i + 1);
            }
        }
        return std::nullopt;
    }

    expected<FrameExtents, WindowError> GLFWWindow::frame_extents() const noexcept {
        ZoneScopedN("GLFWWindow::frame_extents");
        auto lock = glfw_window_mutex().lock();
        if (auto live = require_live_window(window_, "frame_extents"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        glfwGetWindowFrameSize(window_, &left, &top, &right, &bottom);
        return FrameExtents{left, top, right, bottom};
    }

    namespace {
        [[nodiscard]] bool is_text_mime(std::string_view mime_type) noexcept { return mime_type.rfind("text/plain", 0) == 0; }
    } // namespace

    bool GLFWWindow::clipboard_has_mime(std::string_view mime_type) const noexcept {
        if (is_text_mime(mime_type)) {
            return !clipboard_text().empty();
        }
        auto lock = glfw_window_mutex().lock();
        return Detail::native_typed_clipboard_available() && Detail::native_clipboard_has(mime_type);
    }

    std::vector<std::byte> GLFWWindow::clipboard_data(std::string_view mime_type) const noexcept {
        if (is_text_mime(mime_type)) {
            const std::string text = clipboard_text();
            const auto *bytes = reinterpret_cast<const std::byte *>(text.data());
            return std::vector<std::byte>(bytes, bytes + text.size());
        }
        auto lock = glfw_window_mutex().lock();
        return Detail::native_typed_clipboard_available() ? Detail::native_clipboard_get(mime_type) : std::vector<std::byte>{};
    }

    expected<void, WindowError> GLFWWindow::set_clipboard_data(std::string_view mime_type, std::span<const std::byte> bytes) noexcept {
        if (is_text_mime(mime_type)) {
            return set_clipboard_text(std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        }
        auto lock = glfw_window_mutex().lock();
        if (!Detail::native_typed_clipboard_available()) {
            return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW's clipboard is text only on this platform."});
        }
        if (!Detail::native_clipboard_set(window_, mime_type, bytes)) {
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "The platform clipboard rejected the data."});
        }
        return {};
    }

    std::vector<std::string> GLFWWindow::clipboard_mime_types() const noexcept {
        {
            auto lock = glfw_window_mutex().lock();
            if (Detail::native_typed_clipboard_available()) {
                return Detail::native_clipboard_types();
            }
        }
        return clipboard_text().empty() ? std::vector<std::string>{} : std::vector<std::string>{"text/plain;charset=utf-8"};
    }

    expected<void, WindowError> GLFWWindow::set_fullscreen_target(FullscreenTarget target) noexcept {
        ZoneScopedN("GLFWWindow::set_fullscreen_target");
        WindowMode reapply = WindowMode::Windowed;
        {
            auto lock = glfw_window_mutex().lock();
            if (auto live = require_live_window(window_, "set_fullscreen_target"); !live) [[unlikely]] {
                return live;
            }
            fullscreen_target_ = target;
            reapply = fullscreen_mode_;
        }
        // Already fullscreen: move to the new target now. `set_fullscreen` takes the lock itself.
        return reapply == WindowMode::Windowed ? expected<void, WindowError>{} : set_fullscreen(reapply);
    }

} // namespace SFT::WindowManager::GLFW
