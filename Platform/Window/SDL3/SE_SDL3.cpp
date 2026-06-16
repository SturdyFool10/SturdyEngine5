#include "SE_SDL3.hpp"

#include "../WindowEffects.hpp"
#include "../WindowLog.hpp"
#include "../WindowNative.hpp"

#include <SDL3/SDL.h>

#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

namespace SFT::Platform::Windowing::SDL3 {
namespace {

WindowError sdl_error(WindowErrorCode code, const char* fallback) noexcept
{
    const char* message = SDL_GetError();
    return WindowError {code, message && message[0] != '\0' ? message : fallback};
}

[[nodiscard]] bool valid_extent(WindowExtent extent) noexcept
{
    return extent.width > 0 && extent.height > 0
        && extent.width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        && extent.height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
}

[[nodiscard]] SDL_WindowFlags window_flags(const WindowConfig& config) noexcept
{
    SDL_WindowFlags flags = 0;

    if (!config.visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (!config.decorated || config.mode == WindowMode::BorderlessFullscreen) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (config.high_dpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (config.graphics_api == WindowGraphicsApi::Vulkan) {
        flags |= SDL_WINDOW_VULKAN;
    } else if (config.graphics_api == WindowGraphicsApi::OpenGL) {
        flags |= SDL_WINDOW_OPENGL;
    } else if (config.graphics_api == WindowGraphicsApi::Metal) {
        flags |= SDL_WINDOW_METAL;
    }
    if (config.mode != WindowMode::Windowed) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    return flags;
}

WindowResult sdl_bool_result(bool result, WindowErrorCode code, const char* fallback) noexcept
{
    if (result) {
        return {};
    }

    const WindowError error = sdl_error(code, fallback);
    Detail::window_error("SDL3 operation failed: fallback='{}' message='{}' code={}", fallback, error.message, static_cast<int>(error.code));
    return std::unexpected(error);
}

std::recursive_mutex& sdl_window_mutex() noexcept
{
    static std::recursive_mutex mutex;
    return mutex;
}

std::unordered_map<SDL_WindowID, SDL3Window*>& sdl_window_registry() noexcept
{
    static std::unordered_map<SDL_WindowID, SDL3Window*> registry;
    return registry;
}

int& sdl_window_count() noexcept
{
    static int count = 0;
    return count;
}

WindowError destroyed_window_error() noexcept
{
    return WindowError {WindowErrorCode::OperationFailed, "SDL3 window has already been destroyed."};
}

WindowResult require_live_window(SDL_Window* window, const char* operation) noexcept
{
    if (window) {
        return {};
    }

    Detail::window_error("SDL3 operation rejected destroyed window: operation='{}'", operation);
    return std::unexpected(destroyed_window_error());
}

WindowExpected<SDL_WindowID> live_window_id(SDL_Window* window, const char* operation) noexcept
{
    if (!window) {
        Detail::window_error("SDL3 operation rejected destroyed window id query: operation='{}'", operation);
        return std::unexpected(destroyed_window_error());
    }

    return SDL_GetWindowID(window);
}

} // namespace

SDL3Window::SDL3Window(ConstructorKey key, SDL_Window* window) noexcept
    : Window(key)
    , window_(window)
{
}

SDL3Window::~SDL3Window() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());

    if (window_) {
        const SDL_WindowID id = SDL_GetWindowID(window_);
        sdl_window_registry().erase(id);
        Detail::window_info("SDL3 window destroy: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), id);
        SDL_DestroyWindow(window_);
        window_ = nullptr;

        int& count = sdl_window_count();
        if (count > 0) {
            --count;
        }

        if (count == 0) {
            Detail::window_info("SDL3 quit video subsystem after last window destroyed.");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        } else {
            Detail::window_debug("SDL3 window destroyed while other windows remain: remaining_count={}", count);
        }
    } else {
        Detail::window_trace("SDL3 window destroy skipped null native pointer: wrapper={}", static_cast<void*>(this));
    }
}

WindowExpected<std::unique_ptr<SDL3Window>> SDL3Window::construct(ConstructorKey key, const WindowConfig& config) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());

    Detail::window_info(
        "SDL3 window create requested: title='{}' size={}x{} position=({}, {}) default_position={} visible={} resizable={} decorated={} high_dpi={} mode={} graphics_api={}",
        config.title ? config.title : "<null>",
        config.extent.width,
        config.extent.height,
        config.position.x,
        config.position.y,
        config.use_default_position,
        config.visible,
        config.resizable,
        config.decorated,
        config.high_dpi,
        static_cast<int>(config.mode),
        static_cast<int>(config.graphics_api));

    if (!config.title || !valid_extent(config.extent)) {
        Detail::window_error(
            "SDL3 window create rejected invalid config: title_ptr={} size={}x{}",
            static_cast<const void*>(config.title),
            config.extent.width,
            config.extent.height);
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Invalid SDL3 window configuration."});
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        const WindowError error = sdl_error(WindowErrorCode::BackendUnavailable, "SDL3 video subsystem initialization failed.");
        Detail::window_error("SDL3 video subsystem init failed: message='{}'", error.message);
        return std::unexpected(error);
    }
    Detail::window_debug("SDL3 video subsystem initialized or already active.");

    const SDL_WindowFlags flags = window_flags(config);
    Detail::window_debug("SDL3 window flags resolved: flags={}", static_cast<unsigned long long>(flags));
    SDL_Window* window = SDL_CreateWindow(
        config.title,
        static_cast<int>(config.extent.width),
        static_cast<int>(config.extent.height),
        flags);

    if (!window) {
        const WindowError error = sdl_error(WindowErrorCode::CreationFailed, "SDL3 window creation failed.");
        Detail::window_error(
            "SDL3_CreateWindow failed: title='{}' size={}x{} flags={} message='{}'",
            config.title,
            config.extent.width,
            config.extent.height,
            static_cast<unsigned long long>(flags),
            error.message);
        return std::unexpected(error);
    }
    Detail::window_info("SDL3 native window created: ptr={} id={}", static_cast<void*>(window), SDL_GetWindowID(window));

    if (!config.use_default_position) {
        Detail::window_debug("SDL3 setting initial window position: ptr={} x={} y={}", static_cast<void*>(window), config.position.x, config.position.y);
        if (!SDL_SetWindowPosition(window, config.position.x, config.position.y)) {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 window positioning failed.");
            Detail::window_error(
                "SDL3 initial window position failed: ptr={} x={} y={} message='{}'",
                static_cast<void*>(window),
                config.position.x,
                config.position.y,
                error.message);
            SDL_DestroyWindow(window);
            return std::unexpected(error);
        }
    }

    SDL3Window* raw_wrapper = nullptr;
    try {
        raw_wrapper = new SDL3Window(key, window);
        sdl_window_registry().emplace(SDL_GetWindowID(window), raw_wrapper);
        auto wrapper = std::unique_ptr<SDL3Window>(raw_wrapper);
        raw_wrapper = nullptr;
        ++sdl_window_count();
        Detail::window_info("SDL3 window wrapper constructed: wrapper={} native_ptr={} id={}", static_cast<void*>(wrapper.get()), static_cast<void*>(window), SDL_GetWindowID(window));
        return wrapper;
    } catch (const std::bad_alloc&) {
        Detail::window_error("SDL3 window wrapper allocation failed: native_ptr={} id={}", static_cast<void*>(window), SDL_GetWindowID(window));
        if (raw_wrapper) {
            delete raw_wrapper;
        } else {
            SDL_DestroyWindow(window);
        }
        return std::unexpected(WindowError {WindowErrorCode::OutOfMemory, "Out of memory while creating SDL3 window wrapper."});
    }
}

WindowBackendKind SDL3Window::backend_kind() const noexcept
{
    return WindowBackendKind::SDL3;
}

void* SDL3Window::native_backend_handle() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    return window_;
}

NativeWindowHandle SDL3Window::native_window_handle() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (!window_) {
        Detail::window_error("SDL3 native window handle query rejected destroyed window: wrapper={}", static_cast<const void*>(this));
        return {};
    }

    const NativeWindowHandle handle = Detail::native_window_handle_from_sdl(window_);
    Detail::window_debug(
        "SDL3 native window handle queried: wrapper={} native_ptr={} system={} display={} window={}",
        static_cast<const void*>(this),
        static_cast<void*>(window_),
        static_cast<int>(handle.system),
        handle.display,
        handle.window);
    return handle;
}

WindowResult SDL3Window::pump_events() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    const auto maybe_window_id = live_window_id(window_, "pump_events");
    if (!maybe_window_id) {
        return std::unexpected(maybe_window_id.error());
    }

    SDL_Event event;
    const SDL_WindowID window_id = *maybe_window_id;
    int event_count = 0;
    int close_event_count = 0;

    while (SDL_PollEvent(&event)) {
        ++event_count;
        if (event.type == SDL_EVENT_QUIT) {
            for (auto& [registered_id, registered_window] : sdl_window_registry()) {
                if (registered_window) {
                    registered_window->close_requested_.store(true);
                    Detail::window_warn("SDL3 global quit routed to window: target_wrapper={} target_id={}", static_cast<void*>(registered_window), registered_id);
                }
            }
            ++close_event_count;
            Detail::window_warn("SDL3 global quit event observed while pumping window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), window_id);
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (auto found = sdl_window_registry().find(event.window.windowID); found != sdl_window_registry().end() && found->second) {
                found->second->close_requested_.store(true);
                ++close_event_count;
                Detail::window_warn("SDL3 close requested event routed: pump_wrapper={} target_wrapper={} target_id={}", static_cast<void*>(this), static_cast<void*>(found->second), event.window.windowID);
            } else {
                Detail::window_warn("SDL3 close requested event had no registered window: pump_wrapper={} target_id={}", static_cast<void*>(this), event.window.windowID);
            }
        }
    }

    Detail::window_trace(
        "SDL3 event pump complete: wrapper={} native_ptr={} id={} events={} close_events={} close_requested={}",
        static_cast<void*>(this),
        static_cast<void*>(window_),
        window_id,
        event_count,
        close_event_count,
        close_requested_.load());
    return {};
}

bool SDL3Window::close_requested() const noexcept
{
    return close_requested_.load();
}

void SDL3Window::request_close() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    close_requested_.store(true);
    const SDL_WindowID id = window_ ? SDL_GetWindowID(window_) : 0;
    Detail::window_warn("SDL3 close requested by engine: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), id);
}

WindowResult SDL3Window::show() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "show"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 show window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_ShowWindow(window_), WindowErrorCode::OperationFailed, "SDL3 show window failed.");
}

WindowResult SDL3Window::hide() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "hide"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 hide window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_HideWindow(window_), WindowErrorCode::OperationFailed, "SDL3 hide window failed.");
}

WindowResult SDL3Window::focus() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "focus"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 focus window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_RaiseWindow(window_), WindowErrorCode::OperationFailed, "SDL3 focus window failed.");
}

WindowResult SDL3Window::raise() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "raise"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 raise window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_RaiseWindow(window_), WindowErrorCode::OperationFailed, "SDL3 raise window failed.");
}

WindowResult SDL3Window::maximize() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "maximize"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 maximize window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_MaximizeWindow(window_), WindowErrorCode::OperationFailed, "SDL3 maximize window failed.");
}

WindowResult SDL3Window::minimize() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "minimize"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 minimize window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_MinimizeWindow(window_), WindowErrorCode::OperationFailed, "SDL3 minimize window failed.");
}

WindowResult SDL3Window::restore() noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "restore"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 restore window: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
    return sdl_bool_result(SDL_RestoreWindow(window_), WindowErrorCode::OperationFailed, "SDL3 restore window failed.");
}

WindowResult SDL3Window::set_title(const char* title) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_title"); !live) {
        return live;
    }
    if (!title) {
        Detail::window_error("SDL3 set title rejected null title: wrapper={} native_ptr={} id={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_));
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Window title cannot be null."});
    }

    Detail::window_info("SDL3 set title: wrapper={} native_ptr={} id={} title='{}'", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), title);
    return sdl_bool_result(SDL_SetWindowTitle(window_, title), WindowErrorCode::OperationFailed, "SDL3 set title failed.");
}

WindowExpected<WindowPosition> SDL3Window::position() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "position"); !live) {
        return std::unexpected(live.error());
    }
    int x = 0;
    int y = 0;

    if (!SDL_GetWindowPosition(window_, &x, &y)) {
        const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get window position failed.");
        Detail::window_error("SDL3 get position failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), error.message);
        return std::unexpected(error);
    }

    Detail::window_trace("SDL3 get position: wrapper={} native_ptr={} id={} x={} y={}", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), x, y);
    return WindowPosition {x, y};
}

WindowResult SDL3Window::set_position(WindowPosition position) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_position"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set position: wrapper={} native_ptr={} id={} x={} y={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), position.x, position.y);
    return sdl_bool_result(SDL_SetWindowPosition(window_, position.x, position.y), WindowErrorCode::OperationFailed, "SDL3 set position failed.");
}

WindowExpected<WindowExtent> SDL3Window::size() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "size"); !live) {
        return std::unexpected(live.error());
    }
    int width = 0;
    int height = 0;

    if (!SDL_GetWindowSize(window_, &width, &height)) {
        const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get window size failed.");
        Detail::window_error("SDL3 get size failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), error.message);
        return std::unexpected(error);
    }

    Detail::window_trace("SDL3 get size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), width, height);
    return WindowExtent {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

WindowResult SDL3Window::set_size(WindowExtent extent) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_size"); !live) {
        return live;
    }
    if (!valid_extent(extent)) {
        Detail::window_error("SDL3 set size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Window size must be positive and fit in an int."});
    }

    Detail::window_debug("SDL3 set size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
    return sdl_bool_result(
        SDL_SetWindowSize(window_, static_cast<int>(extent.width), static_cast<int>(extent.height)),
        WindowErrorCode::OperationFailed,
        "SDL3 set window size failed.");
}

WindowExpected<WindowExtent> SDL3Window::framebuffer_size() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "framebuffer_size"); !live) {
        return std::unexpected(live.error());
    }
    int width = 0;
    int height = 0;

    if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
        const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get framebuffer size failed.");
        Detail::window_error("SDL3 get framebuffer size failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), error.message);
        return std::unexpected(error);
    }

    Detail::window_trace("SDL3 get framebuffer size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), width, height);
    return WindowExtent {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

WindowResult SDL3Window::set_minimum_size(WindowExtent extent) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_minimum_size"); !live) {
        return live;
    }
    if (!valid_extent(extent)) {
        Detail::window_error("SDL3 set minimum size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Minimum size must be positive and fit in an int."});
    }

    Detail::window_debug("SDL3 set minimum size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
    return sdl_bool_result(
        SDL_SetWindowMinimumSize(window_, static_cast<int>(extent.width), static_cast<int>(extent.height)),
        WindowErrorCode::OperationFailed,
        "SDL3 set minimum size failed.");
}

WindowResult SDL3Window::set_maximum_size(WindowExtent extent) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_maximum_size"); !live) {
        return live;
    }
    if (!valid_extent(extent)) {
        Detail::window_error("SDL3 set maximum size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Maximum size must be positive and fit in an int."});
    }

    Detail::window_debug("SDL3 set maximum size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), extent.width, extent.height);
    return sdl_bool_result(
        SDL_SetWindowMaximumSize(window_, static_cast<int>(extent.width), static_cast<int>(extent.height)),
        WindowErrorCode::OperationFailed,
        "SDL3 set maximum size failed.");
}

WindowResult SDL3Window::set_resizable(bool enabled) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_resizable"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set resizable: wrapper={} native_ptr={} id={} enabled={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), enabled);
    return sdl_bool_result(SDL_SetWindowResizable(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set resizable failed.");
}

WindowResult SDL3Window::set_decorated(bool enabled) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_decorated"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set decorated: wrapper={} native_ptr={} id={} enabled={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), enabled);
    return sdl_bool_result(SDL_SetWindowBordered(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set decorations failed.");
}

WindowResult SDL3Window::set_fullscreen(WindowMode mode) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_fullscreen"); !live) {
        return live;
    }
    Detail::window_info("SDL3 set fullscreen: wrapper={} native_ptr={} id={} mode={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), static_cast<int>(mode));
    return sdl_bool_result(SDL_SetWindowFullscreen(window_, mode != WindowMode::Windowed), WindowErrorCode::OperationFailed, "SDL3 set fullscreen failed.");
}

WindowResult SDL3Window::set_opacity(float opacity) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_opacity"); !live) {
        return live;
    }
    if (opacity < 0.0F || opacity > 1.0F) {
        Detail::window_error("SDL3 set opacity rejected invalid opacity: wrapper={} native_ptr={} opacity={}", static_cast<void*>(this), static_cast<void*>(window_), opacity);
        return std::unexpected(WindowError {WindowErrorCode::InvalidArgument, "Window opacity must be between 0.0 and 1.0."});
    }
    Detail::window_debug("SDL3 set opacity: wrapper={} native_ptr={} id={} opacity={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), opacity);
    return sdl_bool_result(SDL_SetWindowOpacity(window_, opacity), WindowErrorCode::OperationFailed, "SDL3 set opacity failed.");
}

WindowExpected<float> SDL3Window::opacity() const noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "opacity"); !live) {
        return std::unexpected(live.error());
    }
    const float result = SDL_GetWindowOpacity(window_);
    if (result < 0.0F) {
        const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get opacity failed.");
        Detail::window_error("SDL3 get opacity failed: wrapper={} native_ptr={} id={} result={} message='{}'", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), result, error.message);
        return std::unexpected(error);
    }

    Detail::window_trace("SDL3 get opacity: wrapper={} native_ptr={} id={} opacity={}", static_cast<const void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), result);
    return result;
}

WindowResult SDL3Window::set_cursor_visible(bool visible) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_cursor_visible"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set cursor visible: wrapper={} native_ptr={} id={} visible={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), visible);
    return sdl_bool_result(visible ? SDL_ShowCursor() : SDL_HideCursor(), WindowErrorCode::OperationFailed, "SDL3 set cursor visibility failed.");
}

WindowResult SDL3Window::set_cursor_grabbed(bool grabbed) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_cursor_grabbed"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set cursor grabbed: wrapper={} native_ptr={} id={} grabbed={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), grabbed);
    return sdl_bool_result(SDL_SetWindowMouseGrab(window_, grabbed), WindowErrorCode::OperationFailed, "SDL3 set cursor grab failed.");
}

WindowResult SDL3Window::set_relative_mouse_mode(bool enabled) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (auto live = require_live_window(window_, "set_relative_mouse_mode"); !live) {
        return live;
    }
    Detail::window_debug("SDL3 set relative mouse mode: wrapper={} native_ptr={} id={} enabled={}", static_cast<void*>(this), static_cast<void*>(window_), SDL_GetWindowID(window_), enabled);
    return sdl_bool_result(SDL_SetWindowRelativeMouseMode(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set relative mouse mode failed.");
}

WindowEffectResult SDL3Window::enable_window_effect(WindowEffect effect) noexcept
{
    const std::lock_guard lock(sdl_window_mutex());
    if (!window_) {
        Detail::window_error("SDL3 enable native window effect rejected destroyed window: wrapper={} kind={}", static_cast<void*>(this), static_cast<int>(effect.kind));
        return WindowEffectResult::failed("SDL3 window has already been destroyed.");
    }
    Detail::window_info(
        "SDL3 enable native window effect: wrapper={} native_ptr={} id={} kind={} enabled={} color_argb=0x{:08X} linux_blur_protocol={}",
        static_cast<void*>(this),
        static_cast<void*>(window_),
        SDL_GetWindowID(window_),
        static_cast<int>(effect.kind),
        effect.enabled,
        effect.color_argb,
        static_cast<int>(effect.linux_blur_protocol));
    return enable_native_window_effect(native_window_handle(), effect);
}

WindowResult SDL3Window::set_effect(WindowEffect effect) noexcept
{
    return window_result_from_effect_result(enable_window_effect(effect));
}

WindowResult SDL3Window::set_blur_enabled(bool enabled) noexcept
{
    return set_effect(WindowEffect::blur(enabled));
}

} // namespace SFT::Platform::Windowing::SDL3
