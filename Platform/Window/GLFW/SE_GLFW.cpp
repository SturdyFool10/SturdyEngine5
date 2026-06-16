#include "SE_GLFW.hpp"

#include "../WindowEffects.hpp"
#include "../WindowLog.hpp"
#include "../WindowNative.hpp"

#include <GLFW/glfw3.h>

#include <limits>
#include <mutex>
#include <new>

namespace SFT::Platform::Windowing::GLFW {
namespace {

WindowError glfw_error(WindowErrorCode code, const char *fallback) noexcept {
  const char *description = nullptr;
  glfwGetError(&description);
  return WindowError{code, description && description[0] != '\0' ? description
                                                                 : fallback};
}

[[nodiscard]] bool valid_extent(WindowExtent extent) noexcept {
  return extent.width > 0 && extent.height > 0 &&
         extent.width <=
             static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
         extent.height <=
             static_cast<std::uint32_t>(std::numeric_limits<int>::max());
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

  if (config.high_dpi) {
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
  }
}

[[nodiscard]] GLFWmonitor *monitor_for_mode(WindowMode mode) noexcept {
  if (mode == WindowMode::Windowed) {
    return nullptr;
  }

  return glfwGetPrimaryMonitor();
}

WindowResult glfw_success() noexcept {
  const int code = glfwGetError(nullptr);
  if (code == GLFW_NO_ERROR) {
    return {};
  }

  const WindowError error =
      glfw_error(WindowErrorCode::OperationFailed, "GLFW operation failed.");
  Detail::window_error("GLFW operation failed: glfw_code={} message='{}'", code,
                       error.message);
  return std::unexpected(error);
}

std::recursive_mutex &glfw_window_mutex() noexcept {
  static std::recursive_mutex mutex;
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

WindowResult require_live_window(GLFWwindow *window,
                                 const char *operation) noexcept {
  if (window) {
    return {};
  }

  Detail::window_error(
      "GLFW operation rejected destroyed window: operation='{}'", operation);
  return std::unexpected(destroyed_window_error());
}

} // namespace

GLFWWindow::GLFWWindow(ConstructorKey key, GLFWwindow *window) noexcept
    : Window(key), window_(window) {}

GLFWWindow::~GLFWWindow() noexcept {
  const std::lock_guard lock(glfw_window_mutex());

  if (!window_) {
    Detail::window_trace(
        "GLFW window destroy skipped null native pointer: wrapper={}",
        static_cast<void *>(this));
    return;
  }

  Detail::window_info("GLFW window destroy: wrapper={} native_ptr={}",
                      static_cast<void *>(this), static_cast<void *>(window_));
  glfwDestroyWindow(window_);
  window_ = nullptr;

  int &count = glfw_window_count();
  if (count > 0) {
    --count;
  }

  if (count == 0) {
    Detail::window_info("GLFW terminate after last window destroyed.");
    glfwTerminate();
  } else {
    Detail::window_debug(
        "GLFW window destroyed while other windows remain: remaining_count={}",
        count);
  }
}

WindowExpected<std::unique_ptr<GLFWWindow>>
GLFWWindow::construct(ConstructorKey key, const WindowConfig &config) noexcept {
  const std::lock_guard lock(glfw_window_mutex());

  Detail::window_info(
      "GLFW window create requested: title='{}' size={}x{} position=({}, {}) "
      "default_position={} visible={} resizable={} decorated={} high_dpi={} "
      "mode={} graphics_api={} existing_windows={}",
      config.title ? config.title : "<null>", config.extent.width,
      config.extent.height, config.position.x, config.position.y,
      config.use_default_position, config.visible, config.resizable,
      config.decorated, config.high_dpi, static_cast<int>(config.mode),
      static_cast<int>(config.graphics_api), glfw_window_count());

  if (!config.title || !valid_extent(config.extent)) {
    Detail::window_error(
        "GLFW window create rejected invalid config: title_ptr={} size={}x{}",
        static_cast<const void *>(config.title), config.extent.width,
        config.extent.height);
    return std::unexpected(WindowError{WindowErrorCode::InvalidArgument,
                                       "Invalid GLFW window configuration."});
  }

  if (config.graphics_api == WindowGraphicsApi::Metal ||
      config.graphics_api == WindowGraphicsApi::Direct3D) {
    Detail::window_error(
        "GLFW window create rejected unsupported graphics_api={}: title='{}'",
        static_cast<int>(config.graphics_api), config.title);
    return std::unexpected(
        WindowError{WindowErrorCode::Unsupported,
                    "GLFW windows use native handles for Metal and Direct3D; "
                    "request WindowGraphicsApi::None."});
  }

  if (!glfwInit()) {
    const WindowError error = glfw_error(WindowErrorCode::BackendUnavailable,
                                         "GLFW initialization failed.");
    Detail::window_error("GLFW initialization failed: message='{}'",
                         error.message);
    return std::unexpected(error);
  }
  Detail::window_debug("GLFW initialized or already active.");

  apply_window_hints(config);
  Detail::window_debug(
      "GLFW window hints applied: visible={} resizable={} decorated={} "
      "high_dpi={} client_api={}",
      config.visible, config.resizable, config.decorated, config.high_dpi,
      config.graphics_api == WindowGraphicsApi::OpenGL ? "OpenGL" : "NoApi");

  GLFWmonitor *monitor = monitor_for_mode(config.mode);
  Detail::window_debug("GLFW monitor selected for create: mode={} monitor={}",
                       static_cast<int>(config.mode),
                       static_cast<void *>(monitor));
  GLFWwindow *window = glfwCreateWindow(static_cast<int>(config.extent.width),
                                        static_cast<int>(config.extent.height),
                                        config.title, monitor, nullptr);

  if (!window) {
    const WindowError error = glfw_error(WindowErrorCode::CreationFailed,
                                         "GLFW window creation failed.");
    Detail::window_error("glfwCreateWindow failed: title='{}' size={}x{} "
                         "mode={} monitor={} message='{}'",
                         config.title, config.extent.width,
                         config.extent.height, static_cast<int>(config.mode),
                         static_cast<void *>(monitor), error.message);
    return std::unexpected(error);
  }
  Detail::window_info(
      "GLFW native window created: ptr={} title='{}' size={}x{}",
      static_cast<void *>(window), config.title, config.extent.width,
      config.extent.height);

  if (!config.use_default_position && config.mode == WindowMode::Windowed) {
    Detail::window_debug(
        "GLFW setting initial window position: ptr={} x={} y={}",
        static_cast<void *>(window), config.position.x, config.position.y);
    glfwSetWindowPos(window, config.position.x, config.position.y);
  } else if (!config.use_default_position) {
    Detail::window_debug(
        "GLFW skipped initial position because window is fullscreen: ptr={} "
        "mode={} requested_x={} requested_y={}",
        static_cast<void *>(window), static_cast<int>(config.mode),
        config.position.x, config.position.y);
  }

  try {
    auto wrapper = std::unique_ptr<GLFWWindow>(new GLFWWindow(key, window));
    ++glfw_window_count();
    Detail::window_info("GLFW window wrapper constructed: wrapper={} "
                        "native_ptr={} active_count={}",
                        static_cast<void *>(wrapper.get()),
                        static_cast<void *>(window), glfw_window_count());
    return wrapper;
  } catch (const std::bad_alloc &) {
    Detail::window_error("GLFW window wrapper allocation failed: native_ptr={}",
                         static_cast<void *>(window));
    glfwDestroyWindow(window);
    return std::unexpected(
        WindowError{WindowErrorCode::OutOfMemory,
                    "Out of memory while creating GLFW window wrapper."});
  }
}

WindowBackendKind GLFWWindow::backend_kind() const noexcept {
  return WindowBackendKind::GLFW;
}

void *GLFWWindow::native_backend_handle() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  return window_;
}

NativeWindowHandle GLFWWindow::native_window_handle() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (!window_) {
    Detail::window_error(
        "GLFW native window handle query rejected destroyed window: wrapper={}",
        static_cast<const void *>(this));
    return {};
  }

  const NativeWindowHandle handle =
      Detail::native_window_handle_from_glfw(window_);
  Detail::window_debug(
      "GLFW native window handle queried: wrapper={} native_ptr={} system={} "
      "display={} window={}",
      static_cast<const void *>(this), static_cast<void *>(window_),
      static_cast<int>(handle.system), handle.display, handle.window);
  return handle;
}

WindowResult GLFWWindow::pump_events() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "pump_events"); !live) {
    return live;
  }
  Detail::window_trace("GLFW poll events begin: wrapper={} native_ptr={} "
                       "close_requested_before={}",
                       static_cast<void *>(this), static_cast<void *>(window_),
                       close_requested());
  glfwPollEvents();
  Detail::window_trace("GLFW poll events complete: wrapper={} native_ptr={} "
                       "close_requested_after={}",
                       static_cast<void *>(this), static_cast<void *>(window_),
                       close_requested());
  return {};
}

bool GLFWWindow::close_requested() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (!window_) {
    Detail::window_warn(
        "GLFW close_requested queried after destroy: wrapper={}",
        static_cast<const void *>(this));
    return true;
  }
  return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void GLFWWindow::request_close() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (!window_) {
    Detail::window_warn(
        "GLFW request close ignored destroyed window: wrapper={}",
        static_cast<void *>(this));
    return;
  }
  Detail::window_warn(
      "GLFW close requested by engine: wrapper={} native_ptr={}",
      static_cast<void *>(this), static_cast<void *>(window_));
  glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

WindowResult GLFWWindow::show() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "show"); !live) {
    return live;
  }
  Detail::window_debug("GLFW show window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwShowWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::hide() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "hide"); !live) {
    return live;
  }
  Detail::window_debug("GLFW hide window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwHideWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::focus() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "focus"); !live) {
    return live;
  }
  Detail::window_debug("GLFW focus window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwFocusWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::raise() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "raise"); !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW request window attention: wrapper={} native_ptr={}",
      static_cast<void *>(this), static_cast<void *>(window_));
  glfwRequestWindowAttention(window_);
  return glfw_success();
}

WindowResult GLFWWindow::maximize() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "maximize"); !live) {
    return live;
  }
  Detail::window_debug("GLFW maximize window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwMaximizeWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::minimize() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "minimize"); !live) {
    return live;
  }
  Detail::window_debug("GLFW minimize/iconify window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwIconifyWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::restore() noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "restore"); !live) {
    return live;
  }
  Detail::window_debug("GLFW restore window: wrapper={} native_ptr={}",
                       static_cast<void *>(this), static_cast<void *>(window_));
  glfwRestoreWindow(window_);
  return glfw_success();
}

WindowResult GLFWWindow::set_title(const char *title) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_title"); !live) {
    return live;
  }
  if (!title) {
    Detail::window_error(
        "GLFW set title rejected null title: wrapper={} native_ptr={}",
        static_cast<void *>(this), static_cast<void *>(window_));
    return std::unexpected(WindowError{WindowErrorCode::InvalidArgument,
                                       "Window title cannot be null."});
  }

  Detail::window_info("GLFW set title: wrapper={} native_ptr={} title='{}'",
                      static_cast<void *>(this), static_cast<void *>(window_),
                      title);
  glfwSetWindowTitle(window_, title);
  return glfw_success();
}

WindowExpected<WindowPosition> GLFWWindow::position() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "position"); !live) {
    return std::unexpected(live.error());
  }
  int x = 0;
  int y = 0;
  glfwGetWindowPos(window_, &x, &y);
  Detail::window_trace("GLFW get position: wrapper={} native_ptr={} x={} y={}",
                       static_cast<const void *>(this),
                       static_cast<void *>(window_), x, y);
  return WindowPosition{x, y};
}

WindowResult GLFWWindow::set_position(WindowPosition position) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_position"); !live) {
    return live;
  }
  Detail::window_debug("GLFW set position: wrapper={} native_ptr={} x={} y={}",
                       static_cast<void *>(this), static_cast<void *>(window_),
                       position.x, position.y);
  glfwSetWindowPos(window_, position.x, position.y);
  return glfw_success();
}

WindowExpected<WindowExtent> GLFWWindow::size() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "size"); !live) {
    return std::unexpected(live.error());
  }
  int width = 0;
  int height = 0;
  glfwGetWindowSize(window_, &width, &height);
  Detail::window_trace(
      "GLFW get size: wrapper={} native_ptr={} width={} height={}",
      static_cast<const void *>(this), static_cast<void *>(window_), width,
      height);
  return WindowExtent{static_cast<std::uint32_t>(width),
                      static_cast<std::uint32_t>(height)};
}

WindowResult GLFWWindow::set_size(WindowExtent extent) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_size"); !live) {
    return live;
  }
  if (!valid_extent(extent)) {
    Detail::window_error("GLFW set size rejected invalid extent: wrapper={} "
                         "native_ptr={} width={} height={}",
                         static_cast<void *>(this),
                         static_cast<void *>(window_), extent.width,
                         extent.height);
    return std::unexpected(
        WindowError{WindowErrorCode::InvalidArgument,
                    "Window size must be positive and fit in an int."});
  }

  Detail::window_debug(
      "GLFW set size: wrapper={} native_ptr={} width={} height={}",
      static_cast<void *>(this), static_cast<void *>(window_), extent.width,
      extent.height);
  glfwSetWindowSize(window_, static_cast<int>(extent.width),
                    static_cast<int>(extent.height));
  return glfw_success();
}

WindowExpected<WindowExtent> GLFWWindow::framebuffer_size() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "framebuffer_size"); !live) {
    return std::unexpected(live.error());
  }
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  Detail::window_trace(
      "GLFW get framebuffer size: wrapper={} native_ptr={} width={} height={}",
      static_cast<const void *>(this), static_cast<void *>(window_), width,
      height);
  return WindowExtent{static_cast<std::uint32_t>(width),
                      static_cast<std::uint32_t>(height)};
}

WindowResult GLFWWindow::set_minimum_size(WindowExtent extent) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_minimum_size"); !live) {
    return live;
  }
  if (!valid_extent(extent)) {
    Detail::window_error("GLFW set minimum size rejected invalid extent: "
                         "wrapper={} native_ptr={} width={} height={}",
                         static_cast<void *>(this),
                         static_cast<void *>(window_), extent.width,
                         extent.height);
    return std::unexpected(
        WindowError{WindowErrorCode::InvalidArgument,
                    "Minimum size must be positive and fit in an int."});
  }

  Detail::window_debug(
      "GLFW set minimum size: wrapper={} native_ptr={} width={} height={}",
      static_cast<void *>(this), static_cast<void *>(window_), extent.width,
      extent.height);
  glfwSetWindowSizeLimits(window_, static_cast<int>(extent.width),
                          static_cast<int>(extent.height), GLFW_DONT_CARE,
                          GLFW_DONT_CARE);
  return glfw_success();
}

WindowResult GLFWWindow::set_maximum_size(WindowExtent extent) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_maximum_size"); !live) {
    return live;
  }
  if (!valid_extent(extent)) {
    Detail::window_error("GLFW set maximum size rejected invalid extent: "
                         "wrapper={} native_ptr={} width={} height={}",
                         static_cast<void *>(this),
                         static_cast<void *>(window_), extent.width,
                         extent.height);
    return std::unexpected(
        WindowError{WindowErrorCode::InvalidArgument,
                    "Maximum size must be positive and fit in an int."});
  }

  Detail::window_debug(
      "GLFW set maximum size: wrapper={} native_ptr={} width={} height={}",
      static_cast<void *>(this), static_cast<void *>(window_), extent.width,
      extent.height);
  glfwSetWindowSizeLimits(window_, GLFW_DONT_CARE, GLFW_DONT_CARE,
                          static_cast<int>(extent.width),
                          static_cast<int>(extent.height));
  return glfw_success();
}

WindowResult GLFWWindow::set_resizable(bool enabled) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_resizable"); !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW set resizable: wrapper={} native_ptr={} enabled={}",
      static_cast<void *>(this), static_cast<void *>(window_), enabled);
  glfwSetWindowAttrib(window_, GLFW_RESIZABLE,
                      enabled ? GLFW_TRUE : GLFW_FALSE);
  return glfw_success();
}

WindowResult GLFWWindow::set_decorated(bool enabled) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_decorated"); !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW set decorated: wrapper={} native_ptr={} enabled={}",
      static_cast<void *>(this), static_cast<void *>(window_), enabled);
  glfwSetWindowAttrib(window_, GLFW_DECORATED,
                      enabled ? GLFW_TRUE : GLFW_FALSE);
  return glfw_success();
}

WindowResult GLFWWindow::set_fullscreen(WindowMode mode) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_fullscreen"); !live) {
    return live;
  }
  if (mode == WindowMode::Windowed) {
    auto current_size = size();
    const WindowExtent extent = current_size ? *current_size : WindowExtent{};
    Detail::window_info(
        "GLFW set fullscreen/windowed: wrapper={} native_ptr={} mode={} "
        "restored_width={} restored_height={}",
        static_cast<void *>(this), static_cast<void *>(window_),
        static_cast<int>(mode), extent.width, extent.height);
    glfwSetWindowMonitor(window_, nullptr, 0, 0, static_cast<int>(extent.width),
                         static_cast<int>(extent.height), GLFW_DONT_CARE);
    return glfw_success();
  }

  GLFWmonitor *monitor = glfwGetPrimaryMonitor();
  if (!monitor) {
    Detail::window_error("GLFW set fullscreen failed missing primary monitor: "
                         "wrapper={} native_ptr={} mode={}",
                         static_cast<void *>(this),
                         static_cast<void *>(window_), static_cast<int>(mode));
    return std::unexpected(WindowError{WindowErrorCode::OperationFailed,
                                       "GLFW primary monitor is unavailable."});
  }

  const GLFWvidmode *mode_info = glfwGetVideoMode(monitor);
  if (!mode_info) {
    const WindowError error = glfw_error(WindowErrorCode::OperationFailed,
                                         "GLFW video mode is unavailable.");
    Detail::window_error("GLFW set fullscreen failed missing video mode: "
                         "wrapper={} native_ptr={} monitor={} message='{}'",
                         static_cast<void *>(this),
                         static_cast<void *>(window_),
                         static_cast<void *>(monitor), error.message);
    return std::unexpected(error);
  }

  Detail::window_info("GLFW set fullscreen: wrapper={} native_ptr={} mode={} "
                      "monitor={} width={} height={} refresh_rate={}",
                      static_cast<void *>(this), static_cast<void *>(window_),
                      static_cast<int>(mode), static_cast<void *>(monitor),
                      mode_info->width, mode_info->height,
                      mode_info->refreshRate);
  glfwSetWindowMonitor(window_, monitor, 0, 0, mode_info->width,
                       mode_info->height, mode_info->refreshRate);
  return glfw_success();
}

WindowResult GLFWWindow::set_opacity(float opacity) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_opacity"); !live) {
    return live;
  }
  if (opacity < 0.0F || opacity > 1.0F) {
    Detail::window_error("GLFW set opacity rejected invalid opacity: "
                         "wrapper={} native_ptr={} opacity={}",
                         static_cast<void *>(this),
                         static_cast<void *>(window_), opacity);
    return std::unexpected(
        WindowError{WindowErrorCode::InvalidArgument,
                    "Window opacity must be between 0.0 and 1.0."});
  }
  Detail::window_debug("GLFW set opacity: wrapper={} native_ptr={} opacity={}",
                       static_cast<void *>(this), static_cast<void *>(window_),
                       opacity);
  glfwSetWindowOpacity(window_, opacity);
  return glfw_success();
}

WindowExpected<float> GLFWWindow::opacity() const noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "opacity"); !live) {
    return std::unexpected(live.error());
  }
  const float result = glfwGetWindowOpacity(window_);
  Detail::window_trace("GLFW get opacity: wrapper={} native_ptr={} opacity={}",
                       static_cast<const void *>(this),
                       static_cast<void *>(window_), result);
  return result;
}

WindowResult GLFWWindow::set_cursor_visible(bool visible) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_cursor_visible"); !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW set cursor visible: wrapper={} native_ptr={} visible={}",
      static_cast<void *>(this), static_cast<void *>(window_), visible);
  glfwSetInputMode(window_, GLFW_CURSOR,
                   visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
  return glfw_success();
}

WindowResult GLFWWindow::set_cursor_grabbed(bool grabbed) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_cursor_grabbed"); !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW set cursor grabbed: wrapper={} native_ptr={} grabbed={}",
      static_cast<void *>(this), static_cast<void *>(window_), grabbed);
  glfwSetInputMode(window_, GLFW_CURSOR,
                   grabbed ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  return glfw_success();
}

WindowResult GLFWWindow::set_relative_mouse_mode(bool enabled) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (auto live = require_live_window(window_, "set_relative_mouse_mode");
      !live) {
    return live;
  }
  Detail::window_debug(
      "GLFW set relative mouse mode: wrapper={} native_ptr={} enabled={}",
      static_cast<void *>(this), static_cast<void *>(window_), enabled);
  glfwSetInputMode(window_, GLFW_CURSOR,
                   enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  return glfw_success();
}

WindowEffectResult
GLFWWindow::enable_window_effect(WindowEffect effect) noexcept {
  const std::lock_guard lock(glfw_window_mutex());
  if (!window_) {
    Detail::window_error("GLFW enable native window effect rejected destroyed "
                         "window: wrapper={} kind={}",
                         static_cast<void *>(this),
                         static_cast<int>(effect.kind));
    return WindowEffectResult::failed(
        "GLFW window has already been destroyed.");
  }
  Detail::window_info(
      "GLFW enable native window effect: wrapper={} native_ptr={} kind={} "
      "enabled={} color_argb=0x{:08X} linux_blur_protocol={}",
      static_cast<void *>(this), static_cast<void *>(window_),
      static_cast<int>(effect.kind), effect.enabled, effect.color_argb,
      static_cast<int>(effect.linux_blur_protocol));
  return enable_native_window_effect(native_window_handle(), effect);
}

WindowResult GLFWWindow::set_effect(WindowEffect effect) noexcept {
  return window_result_from_effect_result(enable_window_effect(effect));
}

WindowResult GLFWWindow::set_blur_enabled(bool enabled) noexcept {
  return set_effect(WindowEffect::blur(enabled));
}
} // namespace SFT::Platform::Windowing::GLFW
