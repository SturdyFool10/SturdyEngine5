#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h> // SDL.h does not pull this in; needed for SDL_Vulkan_GetInstanceExtensions

#include <chrono>
#include <cmath>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Platform/Window/Window.hpp>
#include <Platform/Window/SDL3/Window.hpp>
#if defined(__linux__)
#include <Platform/Linux/WaylandColorManagement.hpp>
#endif

#include <tracy/Tracy.hpp>

using std::bad_alloc;
using std::expected;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::memcpy;
using std::unexpected;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

namespace SFT::Platform::Windowing::SDL3 {
    namespace {

        [[nodiscard]] KeyboardKey keyboard_key(SDL_Keycode key) noexcept {
            const KeyboardKey ascii_key = keyboard_key_from_ascii(static_cast<i32>(key));
            if (ascii_key != KeyboardKey::Unknown) {
                return ascii_key;
            }

            switch (key) {
                case SDLK_ESCAPE: return KeyboardKey::Escape;
                case SDLK_INSERT: return KeyboardKey::Insert;
                case SDLK_DELETE: return KeyboardKey::Delete;
                case SDLK_RIGHT: return KeyboardKey::Right;
                case SDLK_LEFT: return KeyboardKey::Left;
                case SDLK_DOWN: return KeyboardKey::Down;
                case SDLK_UP: return KeyboardKey::Up;
                case SDLK_PAGEUP: return KeyboardKey::PageUp;
                case SDLK_PAGEDOWN: return KeyboardKey::PageDown;
                case SDLK_HOME: return KeyboardKey::Home;
                case SDLK_END: return KeyboardKey::End;
                case SDLK_CAPSLOCK: return KeyboardKey::CapsLock;
                case SDLK_SCROLLLOCK: return KeyboardKey::ScrollLock;
                case SDLK_NUMLOCKCLEAR: return KeyboardKey::NumLock;
                case SDLK_PRINTSCREEN: return KeyboardKey::PrintScreen;
                case SDLK_PAUSE: return KeyboardKey::Pause;
                case SDLK_LSHIFT: return KeyboardKey::LeftShift;
                case SDLK_LCTRL: return KeyboardKey::LeftControl;
                case SDLK_LALT: return KeyboardKey::LeftAlt;
                case SDLK_LGUI: return KeyboardKey::LeftSuper;
                case SDLK_RSHIFT: return KeyboardKey::RightShift;
                case SDLK_RCTRL: return KeyboardKey::RightControl;
                case SDLK_RALT: return KeyboardKey::RightAlt;
                case SDLK_RGUI: return KeyboardKey::RightSuper;
                case SDLK_MENU:
                case SDLK_APPLICATION:
                    return KeyboardKey::Menu;
                default: return KeyboardKey::Unknown;
            }
        }

        WindowError sdl_error(WindowErrorCode code, const char *fallback) noexcept {
            const char *message = SDL_GetError();
            return WindowError{code, message && message[0] != '\0' ? message : fallback};
        }

        [[nodiscard]] u64 steady_now_ns() noexcept {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
        }

        // SDL events carry their own SDL_GetTicksNS()-based timestamp (nanoseconds since SDL init),
        // not steady_clock's epoch. `offset_ns` (computed once per pump_events() call via
        // sdl_to_steady_offset_ns() below) converts one into the other without a syscall per event.
        [[nodiscard]] u64 sdl_event_timestamp_ns(Uint64 sdl_timestamp, i64 offset_ns) noexcept {
            return static_cast<u64>(static_cast<i64>(sdl_timestamp) + offset_ns);
        }

        [[nodiscard]] i64 sdl_to_steady_offset_ns() noexcept {
            return static_cast<i64>(steady_now_ns()) - static_cast<i64>(SDL_GetTicksNS());
        }

        [[nodiscard]] const char *graphics_api_name(WindowGraphicsApi api) noexcept {
            switch (api) {
                case WindowGraphicsApi::Vulkan:
                    return "Vulkan";
                case WindowGraphicsApi::OpenGL:
                    return "OpenGL";
                case WindowGraphicsApi::Metal:
                    return "Metal";
                case WindowGraphicsApi::Direct3D:
                    return "Direct3D";
                default:
                    return "None";
            }
        }

        [[nodiscard]] bool valid_extent(WindowExtent extent) noexcept {
            return extent.x > 0 && extent.y > 0 && extent.x <= static_cast<u32>(numeric_limits<i32>::max()) && extent.y <= static_cast<u32>(numeric_limits<i32>::max());
        }

        [[nodiscard]] SDL_WindowFlags window_flags(const WindowConfig &config) noexcept {
            SDL_WindowFlags flags = 0;

            if (!config.visible) [[unlikely]] {
                flags |= SDL_WINDOW_HIDDEN;
            }
            if (config.resizable) [[likely]] {
                flags |= SDL_WINDOW_RESIZABLE;
            }
            if (!config.decorated || config.mode == WindowMode::BorderlessFullscreen) [[unlikely]] {
                flags |= SDL_WINDOW_BORDERLESS;
            }
            if (config.high_dpi) [[likely]] {
                flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
            }
            if (config.graphics_api == WindowGraphicsApi::Vulkan) {
                flags |= SDL_WINDOW_VULKAN;
            } else if (config.graphics_api == WindowGraphicsApi::OpenGL) {
                flags |= SDL_WINDOW_OPENGL;
            } else if (config.graphics_api == WindowGraphicsApi::Metal) {
                flags |= SDL_WINDOW_METAL;
            }
            if (config.mode != WindowMode::Windowed) [[unlikely]] {
                flags |= SDL_WINDOW_FULLSCREEN;
            }
            if (config.transparent) [[unlikely]] {
                flags |= SDL_WINDOW_TRANSPARENT;
            }

            return flags;
        }

        [[nodiscard]] SDL_SystemCursor to_sdl_system_cursor(CursorIcon icon) noexcept {
            switch (icon) {
                case CursorIcon::Default: return SDL_SYSTEM_CURSOR_DEFAULT;
                // SDL3 has no open/closed-hand system cursor — see CursorIcon's own doc comment
                // (Window.hpp) for why both fall back to POINTER instead of doing nothing.
                case CursorIcon::Pointer:
                case CursorIcon::Grab:
                case CursorIcon::Grabbing: return SDL_SYSTEM_CURSOR_POINTER;
                case CursorIcon::Text: return SDL_SYSTEM_CURSOR_TEXT;
                case CursorIcon::ResizeHorizontal: return SDL_SYSTEM_CURSOR_EW_RESIZE;
                case CursorIcon::ResizeVertical: return SDL_SYSTEM_CURSOR_NS_RESIZE;
                case CursorIcon::ResizeNwse: return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
                case CursorIcon::ResizeNesw: return SDL_SYSTEM_CURSOR_NESW_RESIZE;
                case CursorIcon::NotAllowed: return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
            }
            return SDL_SYSTEM_CURSOR_DEFAULT;
        }

        expected<void, WindowError> sdl_bool_result(bool result, WindowErrorCode code, const char *fallback) noexcept {
            if (result) [[likely]] {
                return {};
            }

            const WindowError error = sdl_error(code, fallback);
            Foundation::log_error("SDL3 operation failed: fallback='{}' message='{}' code={}", fallback, error.message, static_cast<i32>(error.code));
            return unexpected(error);
        }

        // Process-wide, not per-window: SDL3's own window/event APIs aren't safe to call
        // concurrently across different SDL3Window instances (pump_events() walks the whole
        // registry, e.g.), so one lock covers all of them. Async::Mutex<std::monostate> rather than
        // a bare mutex for the same reason every other lock in this codebase prefers it — see
        // Async::Mutex's own doc comment — and it's non-recursive, so callers must never lock it
        // twice on the same thread (see enable_window_effect()'s use of native_window_handle_locked()
        // instead of native_window_handle() for exactly that reason).
        Async::Mutex<std::monostate> &sdl_window_mutex() noexcept {
            static Async::Mutex<std::monostate> mutex;
            return mutex;
        }

        unordered_map<SDL_WindowID, SDL3Window *> &sdl_window_registry() noexcept {
            static unordered_map<SDL_WindowID, SDL3Window *> registry;
            return registry;
        }

#if defined(_WIN32)
        // SDL exposes one process-wide Windows message hook. Its callback uses this directory to
        // route WM_SIZING to the corresponding wrapper without storing native handles in Platform's
        // public Window abstraction.
        unordered_map<HWND, SDL3Window *> &sdl_win32_window_registry() noexcept {
            static unordered_map<HWND, SDL3Window *> registry;
            return registry;
        }
#endif

        i32 &sdl_window_count() noexcept {
            static i32 count = 0;
            return count;
        }

        WindowError destroyed_window_error() noexcept {
            return WindowError{WindowErrorCode::OperationFailed, "SDL3 window has already been destroyed."};
        }

        expected<void, WindowError> require_live_window(SDL_Window *window, const char *operation) noexcept {
            if (window) [[likely]] {
                return {};
            }

            Foundation::log_error("SDL3 operation rejected destroyed window: operation='{}'", operation);
            return unexpected(destroyed_window_error());
        }

        expected<SDL_WindowID, WindowError> live_window_id(SDL_Window *window, const char *operation) noexcept {
            if (!window) [[unlikely]] {
                Foundation::log_error("SDL3 operation rejected destroyed window id query: operation='{}'", operation);
                return unexpected(destroyed_window_error());
            }

            return SDL_GetWindowID(window);
        }

    } // namespace

    SDL3Window::SDL3Window(ConstructorKey key, SDL_Window *window) noexcept
        : Window(key), window_(window) {
        ZoneScopedN("SDL3Window::SDL3Window");
        if (window_) [[likely]] {
            i32 width = 0;
            i32 height = 0;
            if (SDL_GetWindowSize(window_, &width, &height)) [[likely]] {
                last_size_ = WindowExtent{static_cast<u32>(width), static_cast<u32>(height)};
            }
            if (SDL_GetWindowSizeInPixels(window_, &width, &height)) [[likely]] {
                last_framebuffer_size_ = WindowExtent{static_cast<u32>(width), static_cast<u32>(height)};
            } else [[unlikely]] {
                last_framebuffer_size_ = last_size_;
            }
            last_live_resize_extent_ = last_framebuffer_size_;
        }
    }

    // SDL's Windows move/resize modal loop sends an EXPOSED event from its timer even when the
    // provider defers its normal resize event until the client extent is committed. Sample the
    // physical extent on those exposes; PIXEL_SIZE_CHANGED remains an immediate fast path whenever
    // SDL does dispatch it during the drag. The callback publishes state only, never renders here.
    bool SDLCALL SDL3Window::sdl_live_resize_watch(void *userdata, SDL_Event *event) noexcept {
        ZoneScopedN("SDL3Window::sdl_live_resize_watch");
#if defined(_WIN32)
        const bool pixel_size_changed = event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
        const bool modal_repaint = event->type == SDL_EVENT_WINDOW_EXPOSED;
        if (!pixel_size_changed && !modal_repaint) {
            return true;
        }

        auto *self = static_cast<SDL3Window *>(userdata);
        // A watch can be invoked while an SDL operation already owns this non-recursive lock (for
        // example, a programmatic resize). In that case the normal pump will deliver the event; do
        // not risk self-deadlocking merely to request an optional live-resize repaint.
        auto lock = sdl_window_mutex().try_lock();
        if (!lock || self->window_ == nullptr ||
            SDL_GetWindowID(self->window_) != event->window.windowID ||
            !self->live_resize_callback_) {
            return true;
        }

        WindowExtent extent{};
        if (pixel_size_changed) {
            if (event->window.data1 <= 0 || event->window.data2 <= 0) {
                return true;
            }
            extent = WindowExtent{
                static_cast<u32>(event->window.data1),
                static_cast<u32>(event->window.data2),
            };
        } else {
            i32 width = 0;
            i32 height = 0;
            if (!SDL_GetWindowSizeInPixels(self->window_, &width, &height) ||
                width <= 0 || height <= 0) {
                return true;
            }
            extent = WindowExtent{static_cast<u32>(width), static_cast<u32>(height)};
        }

        if (extent.x == self->last_live_resize_extent_.x &&
            extent.y == self->last_live_resize_extent_.y) {
            return true;
        }
        self->last_live_resize_extent_ = extent;

        // The live-resize callback contract forbids re-entering Window or the renderer, so calling it
        // under this lock safely serializes callback replacement/destruction without a racy copy of
        // std::function.
        self->live_resize_callback_(extent);
#else
        (void)userdata;
        (void)event;
#endif
        return true;
    }

#if defined(_WIN32)
    // SDL calls this on the Windows event-loop thread before it dispatches a native message. WM_SIZING
    // carries the proposed outer rectangle even when SDL defers its own resize event until the modal
    // loop exits, which is the missing live-resize extent for Vulkan presentation.
    bool SDLCALL SDL3Window::sdl_windows_message_hook(void *userdata, MSG *message) noexcept {
        ZoneScopedN("SDL3Window::sdl_windows_message_hook");
        (void)userdata;
        if (message == nullptr || message->message != WM_SIZING) {
            return true;
        }

        auto lock = sdl_window_mutex().try_lock();
        if (!lock) {
            return true;
        }
        const auto found = sdl_win32_window_registry().find(message->hwnd);
        if (found == sdl_win32_window_registry().end() ||
            found->second == nullptr || !found->second->live_resize_callback_) {
            return true;
        }

        const auto *sizing_rect = reinterpret_cast<const RECT *>(message->lParam);
        if (sizing_rect == nullptr) {
            return true;
        }

        RECT current_window_rect{};
        RECT current_client_rect{};
        if (!GetWindowRect(message->hwnd, &current_window_rect) ||
            !GetClientRect(message->hwnd, &current_client_rect)) {
            return true;
        }

        const LONG current_outer_width = current_window_rect.right - current_window_rect.left;
        const LONG current_outer_height = current_window_rect.bottom - current_window_rect.top;
        const LONG current_client_width = current_client_rect.right - current_client_rect.left;
        const LONG current_client_height = current_client_rect.bottom - current_client_rect.top;
        const LONG non_client_width = current_outer_width - current_client_width;
        const LONG non_client_height = current_outer_height - current_client_height;
        const LONG width = (sizing_rect->right - sizing_rect->left) - non_client_width;
        const LONG height = (sizing_rect->bottom - sizing_rect->top) - non_client_height;
        if (width <= 0 || height <= 0) {
            return true;
        }

        SDL3Window *self = found->second;
        const WindowExtent extent{static_cast<u32>(width), static_cast<u32>(height)};
        if (extent.x == self->last_live_resize_extent_.x &&
            extent.y == self->last_live_resize_extent_.y) {
            return true;
        }
        self->last_live_resize_extent_ = extent;
        self->live_resize_callback_(extent);
        return true;
    }
#endif

    SDL3Window::~SDL3Window() noexcept {
        ZoneScopedN("SDL3Window::~SDL3Window");
        // Unregister before releasing the object. A callback already in progress holds
        // sdl_window_mutex(), so the lock below also waits for it to finish publishing.
        SDL_RemoveEventWatch(sdl_live_resize_watch, this);

        auto lock = sdl_window_mutex().lock();
        live_resize_callback_ = {};
#if defined(_WIN32)
        for (auto it = sdl_win32_window_registry().begin(); it != sdl_win32_window_registry().end();) {
            if (it->second == this) {
                it = sdl_win32_window_registry().erase(it);
            } else {
                ++it;
            }
        }
#endif

        if (current_cursor_ != nullptr) {
            SDL_DestroyCursor(current_cursor_);
            current_cursor_ = nullptr;
        }

        if (window_) [[likely]] {
            const SDL_WindowID id = SDL_GetWindowID(window_);
            sdl_window_registry().erase(id);
            bool released_effects = false;
            if (auto native = ::SFT::Platform::Windowing::Detail::native_window_handle_from_sdl(window_)) {
                release_native_window_effects(*native, sdl_window_count() == 1);
                released_effects = true;
            }
            if (!released_effects && native_effect_handle_) {
                release_native_window_effects(*native_effect_handle_, sdl_window_count() == 1);
            }
            active_blur_effect_.reset();
            native_effect_handle_.reset();
            Foundation::log_info("Closing window via SDL3");
            SDL_DestroyWindow(window_);
            window_ = nullptr;

            i32 &count = sdl_window_count();
            if (count > 0) [[likely]] {
                --count;
            }

            if (count == 0) {
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
            }
        }
    }

    void SDL3Window::set_live_resize_callback(std::function<void(WindowExtent)> callback) noexcept {
        ZoneScopedN("SDL3Window::set_live_resize_callback");
        auto lock = sdl_window_mutex().lock();
        if (live_resize_callback_) {
            SDL_RemoveEventWatch(sdl_live_resize_watch, this);
        }
#if defined(_WIN32)
        for (auto it = sdl_win32_window_registry().begin(); it != sdl_win32_window_registry().end();) {
            if (it->second == this) {
                it = sdl_win32_window_registry().erase(it);
            } else {
                ++it;
            }
        }
#endif
        live_resize_callback_ = std::move(callback);
        if (live_resize_callback_) {
            SDL_AddEventWatch(sdl_live_resize_watch, this);
#if defined(_WIN32)
            // SDL's hook is process-wide, so every registered HWND shares this one dispatcher. It is
            // installed on the event-owning thread, as SDL_SetWindowsMessageHook requires.
            if (auto native = native_window_handle_locked(); native &&
                native->system == NativeWindowSystem::Win32 && native->window != nullptr) {
                sdl_win32_window_registry().emplace(static_cast<HWND>(native->window), this);
                SDL_SetWindowsMessageHook(sdl_windows_message_hook, nullptr);
            }
#endif
        }
    }

    std::string SDL3Window::clipboard_text() const noexcept {
        ZoneScopedN("SDL3Window::clipboard_text");
        if (!SDL_HasClipboardText()) {
            return {};
        }
        char *text = SDL_GetClipboardText();
        if (!text) {
            return {};
        }
        std::string result(text);
        SDL_free(text);
        return result;
    }

    expected<void, WindowError> SDL3Window::set_clipboard_text(std::string_view text) noexcept {
        ZoneScopedN("SDL3Window::set_clipboard_text");
        // SDL_SetClipboardText requires a NUL-terminated C string; `text` (a view) isn't
        // guaranteed to be one, so it's copied into an owned buffer first.
        const std::string owned(text);
        if (!SDL_SetClipboardText(owned.c_str())) {
            return unexpected(sdl_error(WindowErrorCode::OperationFailed, "SDL_SetClipboardText failed."));
        }
        return {};
    }

    expected<void, WindowError> SDL3Window::start_text_input() noexcept {
        ZoneScopedN("SDL3Window::start_text_input");
        if (!SDL_StartTextInput(window_)) [[unlikely]] {
            return unexpected(sdl_error(WindowErrorCode::OperationFailed, "SDL_StartTextInput failed."));
        }
        return {};
    }

    expected<void, WindowError> SDL3Window::stop_text_input() noexcept {
        ZoneScopedN("SDL3Window::stop_text_input");
        if (!SDL_StopTextInput(window_)) [[unlikely]] {
            return unexpected(sdl_error(WindowErrorCode::OperationFailed, "SDL_StopTextInput failed."));
        }
        return {};
    }

    expected<void, WindowError> SDL3Window::set_text_input_area(TextInputArea area) noexcept {
        ZoneScopedN("SDL3Window::set_text_input_area");
        const SDL_Rect rect{
            .x = static_cast<int>(area.x),
            .y = static_cast<int>(area.y),
            .w = static_cast<int>(area.width),
            .h = static_cast<int>(area.height),
        };
        if (!SDL_SetTextInputArea(window_, &rect, static_cast<int>(area.cursor_offset_x))) [[unlikely]] {
            return unexpected(sdl_error(WindowErrorCode::OperationFailed, "SDL_SetTextInputArea failed."));
        }
        return {};
    }

    expected<unique_ptr<SDL3Window>, WindowError> SDL3Window::construct(ConstructorKey key, const WindowConfig &config) noexcept {
        ZoneScopedN("SDL3Window::construct");
        auto lock = sdl_window_mutex().lock();

        Foundation::log_info("Opening window '{}' ({}x{}) via SDL3 [{}]",
                            config.title ? config.title : "<null>",
                            config.extent.x,
                            config.extent.y,
                            graphics_api_name(config.graphics_api));

        if (!config.title || !valid_extent(config.extent)) [[unlikely]] {
            Foundation::log_error(
                "SDL3 window create rejected invalid config: title_ptr={} size={}x{}",
                static_cast<const void *>(config.title),
                config.extent.x,
                config.extent.y);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Invalid SDL3 window configuration."});
        }

        // Explicit, not relying on SDL3's own default: low-latency/competitive-game mouse input
        // (relative mode, engaged via set_relative_mouse_mode()/set_mouse_locked() below) must not
        // have the OS pointer-acceleration curve applied, and a cursor warp must never synthesize a
        // spurious motion event a game would have to filter back out. Both hints can be set anytime,
        // before SDL_InitSubSystem included, and are idempotent to set repeatedly.
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "0");

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::BackendUnavailable, "SDL3 video subsystem initialization failed.");
            Foundation::log_error("SDL3 video subsystem init failed: message='{}'", error.message);
            return unexpected(error);
        }

        const SDL_WindowFlags flags = window_flags(config);
        SDL_Window *window = SDL_CreateWindow(
            config.title,
            static_cast<i32>(config.extent.x),
            static_cast<i32>(config.extent.y),
            flags);

        if (!window) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::CreationFailed, "SDL3 window creation failed.");
            Foundation::log_error(
                "SDL3_CreateWindow failed: title='{}' size={}x{} flags={} message='{}'",
                config.title,
                config.extent.x,
                config.extent.y,
                static_cast<unsigned long long>(flags),
                error.message);
            return unexpected(error);
        }
        if (!config.use_default_position) [[unlikely]] {
            if (!SDL_SetWindowPosition(window, config.position.x, config.position.y)) [[unlikely]] {
                const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 window positioning failed.");
                Foundation::log_error(
                    "SDL3 initial window position failed: ptr={} x={} y={} message='{}'",
                    static_cast<void *>(window),
                    config.position.x,
                    config.position.y,
                    error.message);
                SDL_DestroyWindow(window);
                return unexpected(error);
            }
        }

        // SDL3 does not deliver SDL_EVENT_TEXT_INPUT at all until text input is explicitly started
        // for the window (mobile OSes use this to gate on-screen keyboard/IME popups) — without
        // this, WindowTextInputEvent/Engine::TextInputEvent silently never fire, which is exactly
        // what any text-editing UI (UI::text_input()/text_area()) needs to receive typed
        // characters. Started unconditionally for the window's whole lifetime rather than only
        // while some UI text field has focus: this engine has no cross-package channel from "a
        // UI::TextEditState became focused" back to Platform::Windowing::Window, and every desktop
        // platform's SDL3 backend treats this as a cheap no-op change of internal state (unlike
        // mobile, there's no on-screen keyboard for it to summon).
        if (!SDL_StartTextInput(window)) [[unlikely]] {
            Foundation::log_error("SDL3 SDL_StartTextInput failed for window '{}': {}", config.title, SDL_GetError());
        }

        SDL3Window *raw_wrapper = nullptr;
        try {
            raw_wrapper = new SDL3Window(key, window);
            sdl_window_registry().emplace(SDL_GetWindowID(window), raw_wrapper);
            auto wrapper = unique_ptr<SDL3Window>(raw_wrapper);
            raw_wrapper = nullptr;
            ++sdl_window_count();
            return wrapper;
        } catch (const bad_alloc &) {
            Foundation::log_error("SDL3 window wrapper allocation failed: native_ptr={} id={}", static_cast<void *>(window), SDL_GetWindowID(window));
            if (raw_wrapper) {
                delete raw_wrapper;
            } else {
                SDL_DestroyWindow(window);
            }
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory while creating SDL3 window wrapper."});
        }
    }

    WindowBackendKind SDL3Window::backend_kind() const noexcept {
        ZoneScopedN("SDL3Window::backend_kind");
        return WindowBackendKind::SDL3;
    }

    WindowingSystem SDL3Window::type() const noexcept {
        ZoneScopedN("SDL3Window::type");
        return WindowingSystem::SDL3;
    }

    expected<void *, WindowError> SDL3Window::native_backend_handle() const noexcept {
        ZoneScopedN("SDL3Window::native_backend_handle");
        auto lock = sdl_window_mutex().lock();
        if (!window_) [[unlikely]] {
            Foundation::log_error("SDL3 native backend handle query rejected destroyed window: wrapper={}", static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        return window_;
    }

    expected<NativeWindowHandle, WindowError> SDL3Window::native_window_handle() const noexcept {
        ZoneScopedN("SDL3Window::native_window_handle");
        auto lock = sdl_window_mutex().lock();
        return native_window_handle_locked();
    }

    optional<WindowHdrProperties> SDL3Window::hdr_properties() const noexcept {
        ZoneScopedN("SDL3Window::hdr_properties");
        auto lock = sdl_window_mutex().lock();
        if (!window_) {
            return nullopt;
        }
        const SDL_PropertiesID properties = SDL_GetWindowProperties(window_);
        optional<WindowHdrProperties> result;
        if (properties != 0 &&
            SDL_HasProperty(properties, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT)) {
            const f32 sdr_white_level = SDL_GetFloatProperty(
                properties, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 0.0f);
            if (std::isfinite(sdr_white_level) && sdr_white_level > 0.0f) {
                const f32 hdr_headroom = SDL_GetFloatProperty(
                    properties, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
                result = WindowHdrProperties{
                    .hdr_enabled = SDL_GetBooleanProperty(
                        properties, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false),
                    .sdr_white_level = sdr_white_level,
                    .hdr_headroom = std::isfinite(hdr_headroom) && hdr_headroom > 0.0f
                        ? hdr_headroom
                        : 1.0f,
                };
            }
        }

#if defined(__linux__)
        constexpr u64 reference_white_refresh_interval_ns = 5'000'000'000ull;
        const u64 now_ns = steady_now_ns();
        const bool refresh_wayland_reference = !wayland_reference_white_queried_ ||
            now_ns < wayland_reference_white_query_time_ns_ ||
            now_ns - wayland_reference_white_query_time_ns_ >= reference_white_refresh_interval_ns;
        if (refresh_wayland_reference) {
            wayland_reference_white_queried_ = true;
            wayland_reference_white_query_time_ns_ = now_ns;
            if (auto handle = ::SFT::Platform::Windowing::Detail::native_window_handle_from_sdl(window_);
                handle && handle->system == NativeWindowSystem::Wayland) {
                wayland_reference_white_nits_ =
                    ::SFT::Platform::Windowing::Detail::query_wayland_surface_reference_white(*handle);
            }
        }
        if (wayland_reference_white_nits_ && *wayland_reference_white_nits_ > 0.0f) {
            if (!result) {
                result = WindowHdrProperties{};
            }
            result->hdr_enabled = true;
            result->sdr_white_level = *wayland_reference_white_nits_ / 80.0f;
        }
#endif
        return result;
    }

    expected<NativeWindowHandle, WindowError> SDL3Window::native_window_handle_locked() const noexcept {
        ZoneScopedN("SDL3Window::native_window_handle_locked");
        if (!window_) [[unlikely]] {
            Foundation::log_error("SDL3 native window handle query rejected destroyed window: wrapper={}", static_cast<const void *>(this));
            return unexpected(destroyed_window_error());
        }

        auto handle = ::SFT::Platform::Windowing::Detail::native_window_handle_from_sdl(window_);
        if (!handle) [[unlikely]] {
            return unexpected(handle.error());
        }

        Foundation::log_debug(
            "SDL3 native window handle queried: wrapper={} native_ptr={} system={} display={} window={}",
            static_cast<const void *>(this),
            static_cast<void *>(window_),
            static_cast<i32>(handle->system),
            handle->display,
            handle->window);
        return *handle;
    }

    expected<void, WindowError> SDL3Window::pump_events() noexcept {
        ZoneScopedN("SDL3Window::pump_events");
        SDL_WindowID window_id{};
        {
            auto lock = sdl_window_mutex().lock();
            const auto maybe_window_id = live_window_id(window_, "pump_events");
            if (!maybe_window_id) [[unlikely]] {
                return unexpected(maybe_window_id.error());
            }
            window_id = *maybe_window_id;
        }

        SDL_Event event;
        i32 event_count = 0;
        i32 close_event_count = 0;
        i32 queued_event_count = 0;
        constexpr i32 max_events_per_pump = 128;
        // Computed once per pump rather than per event -- see sdl_to_steady_offset_ns()'s doc comment.
        // A pump batch spans at most a few hundred microseconds, so drift within one batch is
        // negligible next to the nanosecond-timestamp resolution being converted.
        const i64 timestamp_offset_ns = sdl_to_steady_offset_ns();

        // Deliberately do not hold sdl_window_mutex() across SDL_PollEvent. During Windows'
        // interactive move/resize modal loop, sdl_live_resize_watch runs synchronously as SDL queues
        // the pixel-size event and needs this lock to publish the latest extent. Holding it here
        // would make the watch skip that update, leaving the application to redraw only after the
        // modal loop returns.
        while (event_count < max_events_per_pump && SDL_PollEvent(&event)) {
            auto lock = sdl_window_mutex().lock();
            ++event_count;
            // Cheap per-event arithmetic (no syscall) against the once-per-pump offset above.
            const u64 event_timestamp_ns = sdl_event_timestamp_ns(event.common.timestamp, timestamp_offset_ns);
            if (event.type == SDL_EVENT_QUIT) [[unlikely]] {
                for (auto &[registered_id, registered_window] : sdl_window_registry()) {
                    if (registered_window) [[likely]] {
                        registered_window->close_requested_.store(true);
                        WindowEvent close_event{WindowEventKind::CloseRequested};
                        close_event.timestamp_ns = event_timestamp_ns;
                        registered_window->events_.push_back(close_event);
                        Foundation::log_warn("SDL3 global quit routed to window: target_wrapper={} target_id={}", static_cast<void *>(registered_window), registered_id);
                    }
                }
                ++close_event_count;
                Foundation::log_warn("SDL3 global quit event observed while pumping window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), window_id);
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (auto found = sdl_window_registry().find(event.window.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    found->second->close_requested_.store(true);
                    WindowEvent close_event{WindowEventKind::CloseRequested};
                    close_event.timestamp_ns = event_timestamp_ns;
                    found->second->events_.push_back(close_event);
                    ++close_event_count;
                    ++queued_event_count;
                    Foundation::log_warn("SDL3 close requested event routed: pump_wrapper={} target_wrapper={} target_id={}", static_cast<void *>(this), static_cast<void *>(found->second), event.window.windowID);
                } else [[unlikely]] {
                    Foundation::log_warn("SDL3 close requested event had no registered window: pump_wrapper={} target_id={}", static_cast<void *>(this), event.window.windowID);
                }
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                if (auto found = sdl_window_registry().find(event.window.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    SDL3Window *target = found->second;
                    const WindowExtent previous_size = target->last_size_;
                    const WindowExtent previous_framebuffer = target->last_framebuffer_size_;
                    WindowEvent window_event{event.type == SDL_EVENT_WINDOW_RESIZED ? WindowEventKind::Resized : WindowEventKind::FramebufferResized};
                    window_event.timestamp_ns = event_timestamp_ns;

                    if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                        target->last_size_ = WindowExtent{static_cast<u32>(event.window.data1), static_cast<u32>(event.window.data2)};
                    } else {
                        target->last_framebuffer_size_ = WindowExtent{static_cast<u32>(event.window.data1), static_cast<u32>(event.window.data2)};
                        target->last_live_resize_extent_ = target->last_framebuffer_size_;
                    }

                    window_event.resize = WindowResize{
                        previous_size,
                        target->last_size_,
                        target->last_framebuffer_size_,
                        previous_framebuffer.x != target->last_framebuffer_size_.x || previous_framebuffer.y != target->last_framebuffer_size_.y,
                    };
                    target->pending_resize_ = window_event.resize;
                    target->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_WINDOW_MOVED) {
                if (auto found = sdl_window_registry().find(event.window.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    found->second->wayland_reference_white_queried_ = false;
                    found->second->wayland_reference_white_nits_.reset();
                    found->second->wayland_reference_white_query_time_ns_ = 0;
                    WindowEvent window_event{WindowEventKind::Moved};
                    window_event.timestamp_ns = event_timestamp_ns;
                    window_event.position = WindowPosition{event.window.data1, event.window.data2};
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) {
                if (auto found = sdl_window_registry().find(event.window.windowID);
                    found != sdl_window_registry().end() && found->second) {
                    found->second->wayland_reference_white_queried_ = false;
                    found->second->wayland_reference_white_nits_.reset();
                    found->second->wayland_reference_white_query_time_ns_ = 0;
                }
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MOUSE_ENTER || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
                if (auto found = sdl_window_registry().find(event.window.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEventKind kind = WindowEventKind::FocusGained;
                    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                        kind = WindowEventKind::FocusLost;
                    } else if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
                        kind = WindowEventKind::MouseEntered;
                    } else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
                        kind = WindowEventKind::MouseLeft;
                    }
                    WindowEvent state_event{kind};
                    state_event.timestamp_ns = event_timestamp_ns;
                    found->second->events_.push_back(state_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                if (auto found = sdl_window_registry().find(event.key.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{event.type == SDL_EVENT_KEY_DOWN ? WindowEventKind::KeyPressed : WindowEventKind::KeyReleased};
                    window_event.timestamp_ns = event_timestamp_ns;
                    window_event.keyboard = WindowKeyboardEvent{
                        .key = static_cast<i32>(event.key.key),
                        .scancode = static_cast<i32>(event.key.scancode),
                        .modifiers = static_cast<u32>(event.key.mod),
                        .repeat = event.key.repeat,
                        .key_code = keyboard_key(event.key.key),
                    };
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (auto found = sdl_window_registry().find(event.text.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{WindowEventKind::TextInput};
                    window_event.timestamp_ns = event_timestamp_ns;
                    if (event.text.text) [[likely]] {
                        const usize copy_size = SDL_strnlen(event.text.text, sizeof(window_event.text.utf8) - 1);
                        memcpy(window_event.text.utf8, event.text.text, copy_size);
                    }
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_TEXT_EDITING) {
                // The IME composition/preedit update itself — see WindowTextEditingEvent's own doc
                // comment for why an empty text field here means composition ended, and why this
                // needs its own event kind rather than reusing TextInput (uncommitted composition
                // text must never be treated as if the user actually typed it).
                if (auto found = sdl_window_registry().find(event.edit.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{WindowEventKind::TextEditing};
                    window_event.timestamp_ns = event_timestamp_ns;
                    if (event.edit.text) [[likely]] {
                        const usize copy_size = SDL_strnlen(event.edit.text, sizeof(window_event.editing.utf8) - 1);
                        memcpy(window_event.editing.utf8, event.edit.text, copy_size);
                    }
                    window_event.editing.cursor = event.edit.start;
                    window_event.editing.selection_length = event.edit.length;
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (auto found = sdl_window_registry().find(event.motion.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{WindowEventKind::MouseMoved};
                    window_event.timestamp_ns = event_timestamp_ns;
                    window_event.mouse_move = WindowMouseMoveEvent{
                        event.motion.x,
                        event.motion.y,
                        event.motion.xrel,
                        event.motion.yrel,
                        static_cast<u32>(event.motion.state),
                    };
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (auto found = sdl_window_registry().find(event.button.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? WindowEventKind::MouseButtonPressed : WindowEventKind::MouseButtonReleased};
                    window_event.timestamp_ns = event_timestamp_ns;
                    window_event.mouse_button = WindowMouseButtonEvent{
                        .button = event.button.button,
                        .clicks = event.button.clicks,
                        .x = event.button.x,
                        .y = event.button.y,
                        .button_code = Detail::normalize_mouse_button(event.button.button),
                    };
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (auto found = sdl_window_registry().find(event.wheel.windowID); found != sdl_window_registry().end() && found->second) [[likely]] {
                    WindowEvent window_event{WindowEventKind::MouseWheel};
                    window_event.timestamp_ns = event_timestamp_ns;
                    window_event.mouse_wheel = WindowMouseWheelEvent{event.wheel.x, event.wheel.y, event.wheel.mouse_x, event.wheel.mouse_y};
                    found->second->events_.push_back(window_event);
                    ++queued_event_count;
                }
            }
        }

        {
            auto lock = sdl_window_mutex().lock();
            Foundation::log_trace(
                "SDL3 event pump complete: wrapper={} native_ptr={} id={} events={} queued_events={} close_events={} close_requested={}",
                static_cast<void *>(this),
                static_cast<void *>(window_),
                window_id,
                event_count,
                queued_event_count,
                close_event_count,
                close_requested_.load());
        }
        return {};
    }

    optional<WindowEvent> SDL3Window::poll_event() noexcept {
        ZoneScopedN("SDL3Window::poll_event");
        auto lock = sdl_window_mutex().lock();
        if (events_.empty()) {
            return nullopt;
        }

        WindowEvent event = events_.front();
        events_.pop_front();
        return event;
    }

    bool SDL3Window::close_requested() const noexcept {
        ZoneScopedN("SDL3Window::close_requested");
        return close_requested_.load();
    }

    void SDL3Window::request_close() noexcept {
        ZoneScopedN("SDL3Window::request_close");
        auto lock = sdl_window_mutex().lock();
        close_requested_.store(true);
        const SDL_WindowID id = window_ ? SDL_GetWindowID(window_) : 0;
        WindowEvent close_event{WindowEventKind::CloseRequested};
        close_event.timestamp_ns = steady_now_ns();
        events_.push_back(close_event);
        Foundation::log_warn("SDL3 close requested by engine: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), id);
    }

    bool SDL3Window::resized() const noexcept {
        ZoneScopedN("SDL3Window::resized");
        auto lock = sdl_window_mutex().lock();
        return pending_resize_.has_value();
    }

    optional<WindowResize> SDL3Window::consume_resize() noexcept {
        ZoneScopedN("SDL3Window::consume_resize");
        auto lock = sdl_window_mutex().lock();
        optional<WindowResize> resize = pending_resize_;
        pending_resize_.reset();
        return resize;
    }

    expected<void, WindowError> SDL3Window::show() noexcept {
        ZoneScopedN("SDL3Window::show");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "show"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 show window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        auto shown = sdl_bool_result(SDL_ShowWindow(window_), WindowErrorCode::OperationFailed, "SDL3 show window failed.");
        if (!shown) {
            return shown;
        }
        wayland_reference_white_queried_ = false;
        wayland_reference_white_nits_.reset();
        wayland_reference_white_query_time_ns_ = 0;
        if (active_blur_effect_) {
            if (auto handle = native_window_handle_locked()) {
                native_effect_handle_ = *handle;
                const WindowEffectResult applied = enable_native_window_effect(*handle, *active_blur_effect_);
                if (!applied.succeeded()) {
                    Foundation::log_warn("SDL3 could not reapply blur after showing a recreated native surface: {}", applied.details);
                }
            }
        }
        return {};
    }

    expected<void, WindowError> SDL3Window::hide() noexcept {
        ZoneScopedN("SDL3Window::hide");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "hide"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 hide window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        optional<NativeWindowHandle> released_handle;
        if (active_blur_effect_) {
            if (auto handle = native_window_handle_locked()) {
                released_handle = *handle;
                native_effect_handle_ = *handle;
                release_native_window_effects(*handle, false);
            }
        }
        auto hidden = sdl_bool_result(SDL_HideWindow(window_), WindowErrorCode::OperationFailed, "SDL3 hide window failed.");
        if (!hidden && released_handle && active_blur_effect_) {
            (void)enable_native_window_effect(*released_handle, *active_blur_effect_);
        }
        return hidden;
    }

    expected<void, WindowError> SDL3Window::focus() noexcept {
        ZoneScopedN("SDL3Window::focus");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "focus"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 focus window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        return sdl_bool_result(SDL_RaiseWindow(window_), WindowErrorCode::OperationFailed, "SDL3 focus window failed.");
    }

    expected<void, WindowError> SDL3Window::raise() noexcept {
        ZoneScopedN("SDL3Window::raise");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "raise"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 raise window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        return sdl_bool_result(SDL_RaiseWindow(window_), WindowErrorCode::OperationFailed, "SDL3 raise window failed.");
    }

    expected<void, WindowError> SDL3Window::maximize() noexcept {
        ZoneScopedN("SDL3Window::maximize");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "maximize"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 maximize window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        return sdl_bool_result(SDL_MaximizeWindow(window_), WindowErrorCode::OperationFailed, "SDL3 maximize window failed.");
    }

    expected<void, WindowError> SDL3Window::minimize() noexcept {
        ZoneScopedN("SDL3Window::minimize");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "minimize"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 minimize window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        return sdl_bool_result(SDL_MinimizeWindow(window_), WindowErrorCode::OperationFailed, "SDL3 minimize window failed.");
    }

    expected<void, WindowError> SDL3Window::restore() noexcept {
        ZoneScopedN("SDL3Window::restore");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "restore"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 restore window: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
        return sdl_bool_result(SDL_RestoreWindow(window_), WindowErrorCode::OperationFailed, "SDL3 restore window failed.");
    }

    expected<void, WindowError> SDL3Window::set_title(const char *title) noexcept {
        ZoneScopedN("SDL3Window::set_title");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_title"); !live) [[unlikely]] {
            return live;
        }
        if (!title) [[unlikely]] {
            Foundation::log_error("SDL3 set title rejected null title: wrapper={} native_ptr={} id={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_));
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Window title cannot be null."});
        }

        return sdl_bool_result(SDL_SetWindowTitle(window_, title), WindowErrorCode::OperationFailed, "SDL3 set title failed.");
    }

    expected<WindowPosition, WindowError> SDL3Window::position() const noexcept {
        ZoneScopedN("SDL3Window::position");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "position"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        i32 x = 0;
        i32 y = 0;

        if (!SDL_GetWindowPosition(window_, &x, &y)) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get window position failed.");
            Foundation::log_error("SDL3 get position failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), error.message);
            return unexpected(error);
        }

        Foundation::log_trace("SDL3 get position: wrapper={} native_ptr={} id={} x={} y={}", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), x, y);
        return WindowPosition{x, y};
    }

    expected<void, WindowError> SDL3Window::set_position(WindowPosition position) noexcept {
        ZoneScopedN("SDL3Window::set_position");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_position"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set position: wrapper={} native_ptr={} id={} x={} y={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), position.x, position.y);
        return sdl_bool_result(SDL_SetWindowPosition(window_, position.x, position.y), WindowErrorCode::OperationFailed, "SDL3 set position failed.");
    }

    expected<WindowPosition, WindowError> SDL3Window::global_cursor_position() const noexcept {
        ZoneScopedN("SDL3Window::global_cursor_position");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "global_cursor_position"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        const char *video_driver = SDL_GetCurrentVideoDriver();
        const string_view driver = video_driver != nullptr ? string_view{video_driver} : string_view{};
        if (driver == "wayland" || driver == "emscripten") {
            return unexpected(WindowError{
                WindowErrorCode::Unsupported,
                "SDL3 global cursor position is unavailable on this video driver.",
            });
        }

        // A genuine OS-level global query (not keyed to `window_` at all) on SDL drivers that
        // support desktop-global pointer coordinates.
        f32 x = 0.0f;
        f32 y = 0.0f;
        SDL_GetGlobalMouseState(&x, &y);
        return WindowPosition{static_cast<i32>(x), static_cast<i32>(y)};
    }

    expected<WindowExtent, WindowError> SDL3Window::size() const noexcept {
        ZoneScopedN("SDL3Window::size");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "size"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        i32 width = 0;
        i32 height = 0;

        if (!SDL_GetWindowSize(window_, &width, &height)) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get window size failed.");
            Foundation::log_error("SDL3 get size failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), error.message);
            return unexpected(error);
        }

        Foundation::log_trace("SDL3 get size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), width, height);
        return WindowExtent{static_cast<u32>(width), static_cast<u32>(height)};
    }

    expected<void, WindowError> SDL3Window::set_size(WindowExtent extent) noexcept {
        ZoneScopedN("SDL3Window::set_size");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("SDL3 set size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Window size must be positive and fit in an i32."});
        }

        Foundation::log_debug("SDL3 set size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
        return sdl_bool_result(
            SDL_SetWindowSize(window_, static_cast<i32>(extent.x), static_cast<i32>(extent.y)),
            WindowErrorCode::OperationFailed,
            "SDL3 set window size failed.");
    }

    expected<WindowExtent, WindowError> SDL3Window::framebuffer_size() const noexcept {
        ZoneScopedN("SDL3Window::framebuffer_size");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "framebuffer_size"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        i32 width = 0;
        i32 height = 0;

        if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get framebuffer size failed.");
            Foundation::log_error("SDL3 get framebuffer size failed: wrapper={} native_ptr={} id={} message='{}'", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), error.message);
            return unexpected(error);
        }

        Foundation::log_trace("SDL3 get framebuffer size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), width, height);
        return WindowExtent{static_cast<u32>(width), static_cast<u32>(height)};
    }

    expected<void, WindowError> SDL3Window::set_minimum_size(WindowExtent extent) noexcept {
        ZoneScopedN("SDL3Window::set_minimum_size");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_minimum_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("SDL3 set minimum size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Minimum size must be positive and fit in an i32."});
        }

        Foundation::log_debug("SDL3 set minimum size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
        return sdl_bool_result(
            SDL_SetWindowMinimumSize(window_, static_cast<i32>(extent.x), static_cast<i32>(extent.y)),
            WindowErrorCode::OperationFailed,
            "SDL3 set minimum size failed.");
    }

    expected<void, WindowError> SDL3Window::set_maximum_size(WindowExtent extent) noexcept {
        ZoneScopedN("SDL3Window::set_maximum_size");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_maximum_size"); !live) [[unlikely]] {
            return live;
        }
        if (!valid_extent(extent)) [[unlikely]] {
            Foundation::log_error("SDL3 set maximum size rejected invalid extent: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Maximum size must be positive and fit in an i32."});
        }

        Foundation::log_debug("SDL3 set maximum size: wrapper={} native_ptr={} id={} width={} height={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), extent.x, extent.y);
        return sdl_bool_result(
            SDL_SetWindowMaximumSize(window_, static_cast<i32>(extent.x), static_cast<i32>(extent.y)),
            WindowErrorCode::OperationFailed,
            "SDL3 set maximum size failed.");
    }

    expected<void, WindowError> SDL3Window::set_resizable(bool enabled) noexcept {
        ZoneScopedN("SDL3Window::set_resizable");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_resizable"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set resizable: wrapper={} native_ptr={} id={} enabled={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), enabled);
        return sdl_bool_result(SDL_SetWindowResizable(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set resizable failed.");
    }

    expected<void, WindowError> SDL3Window::set_decorated(bool enabled) noexcept {
        ZoneScopedN("SDL3Window::set_decorated");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_decorated"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set decorated: wrapper={} native_ptr={} id={} enabled={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), enabled);
        return sdl_bool_result(SDL_SetWindowBordered(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set decorations failed.");
    }

    expected<void, WindowError> SDL3Window::set_fullscreen(WindowMode mode) noexcept {
        ZoneScopedN("SDL3Window::set_fullscreen");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_fullscreen"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_info("SDL3 set fullscreen: wrapper={} native_ptr={} id={} mode={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), static_cast<i32>(mode));
        return sdl_bool_result(SDL_SetWindowFullscreen(window_, mode != WindowMode::Windowed), WindowErrorCode::OperationFailed, "SDL3 set fullscreen failed.");
    }

    expected<void, WindowError> SDL3Window::set_opacity(f32 opacity) noexcept {
        ZoneScopedN("SDL3Window::set_opacity");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_opacity"); !live) [[unlikely]] {
            return live;
        }
        if (opacity < 0.0F || opacity > 1.0F) [[unlikely]] {
            Foundation::log_error("SDL3 set opacity rejected invalid opacity: wrapper={} native_ptr={} opacity={}", static_cast<void *>(this), static_cast<void *>(window_), opacity);
            return unexpected(WindowError{WindowErrorCode::InvalidArgument, "Window opacity must be between 0.0 and 1.0."});
        }
        Foundation::log_debug("SDL3 set opacity: wrapper={} native_ptr={} id={} opacity={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), opacity);
        return sdl_bool_result(SDL_SetWindowOpacity(window_, opacity), WindowErrorCode::OperationFailed, "SDL3 set opacity failed.");
    }

    expected<f32, WindowError> SDL3Window::opacity() const noexcept {
        ZoneScopedN("SDL3Window::opacity");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "opacity"); !live) [[unlikely]] {
            return unexpected(live.error());
        }
        const f32 result = SDL_GetWindowOpacity(window_);
        if (result < 0.0F) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 get opacity failed.");
            Foundation::log_error("SDL3 get opacity failed: wrapper={} native_ptr={} id={} result={} message='{}'", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), result, error.message);
            return unexpected(error);
        }

        Foundation::log_trace("SDL3 get opacity: wrapper={} native_ptr={} id={} opacity={}", static_cast<const void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), result);
        return result;
    }

    expected<void, WindowError> SDL3Window::set_cursor_visible(bool visible) noexcept {
        ZoneScopedN("SDL3Window::set_cursor_visible");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_visible"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set cursor visible: wrapper={} native_ptr={} id={} visible={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), visible);
        return sdl_bool_result(visible ? SDL_ShowCursor() : SDL_HideCursor(), WindowErrorCode::OperationFailed, "SDL3 set cursor visibility failed.");
    }

    expected<void, WindowError> SDL3Window::set_cursor_icon(CursorIcon icon) noexcept {
        ZoneScopedN("SDL3Window::set_cursor_icon");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_icon"); !live) [[unlikely]] {
            return live;
        }
        // Skip the churn entirely on the overwhelmingly common case (nothing changed since last
        // call) — a caller driving this off per-frame hover state, as UI::Context::desired_cursor()
        // is meant to be, would otherwise recreate a native cursor object every single frame.
        if (current_cursor_icon_.has_value() && *current_cursor_icon_ == icon) {
            return {};
        }
        SDL_Cursor *cursor = SDL_CreateSystemCursor(to_sdl_system_cursor(icon));
        if (cursor == nullptr) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 create system cursor failed.");
            Foundation::log_error("SDL3 set cursor icon failed: wrapper={} native_ptr={} id={} icon={} message='{}'", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), static_cast<i32>(icon), error.message);
            return unexpected(error);
        }
        if (!SDL_SetCursor(cursor)) [[unlikely]] {
            const WindowError error = sdl_error(WindowErrorCode::OperationFailed, "SDL3 set cursor failed.");
            Foundation::log_error("SDL3 set cursor icon failed: wrapper={} native_ptr={} id={} icon={} message='{}'", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), static_cast<i32>(icon), error.message);
            SDL_DestroyCursor(cursor);
            return unexpected(error);
        }
        if (current_cursor_ != nullptr) {
            SDL_DestroyCursor(current_cursor_);
        }
        current_cursor_ = cursor;
        current_cursor_icon_ = icon;
        Foundation::log_trace("SDL3 set cursor icon: wrapper={} native_ptr={} id={} icon={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), static_cast<i32>(icon));
        return {};
    }

    expected<void, WindowError> SDL3Window::set_cursor_grabbed(bool grabbed) noexcept {
        ZoneScopedN("SDL3Window::set_cursor_grabbed");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_cursor_grabbed"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set cursor grabbed: wrapper={} native_ptr={} id={} grabbed={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), grabbed);
        return sdl_bool_result(SDL_SetWindowMouseGrab(window_, grabbed), WindowErrorCode::OperationFailed, "SDL3 set cursor grab failed.");
    }

    expected<void, WindowError> SDL3Window::set_relative_mouse_mode(bool enabled) noexcept {
        ZoneScopedN("SDL3Window::set_relative_mouse_mode");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_relative_mouse_mode"); !live) [[unlikely]] {
            return live;
        }
        Foundation::log_debug("SDL3 set relative mouse mode: wrapper={} native_ptr={} id={} enabled={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), enabled);
        return sdl_bool_result(SDL_SetWindowRelativeMouseMode(window_, enabled), WindowErrorCode::OperationFailed, "SDL3 set relative mouse mode failed.");
    }

    expected<void, WindowError> SDL3Window::set_mouse_locked(bool locked) noexcept {
        ZoneScopedN("SDL3Window::set_mouse_locked");
        auto lock = sdl_window_mutex().lock();
        if (auto live = require_live_window(window_, "set_mouse_locked"); !live) [[unlikely]] {
            return live;
        }

        Foundation::log_debug("SDL3 set mouse locked: wrapper={} native_ptr={} id={} locked={}", static_cast<void *>(this), static_cast<void *>(window_), SDL_GetWindowID(window_), locked);
        if (auto result = sdl_bool_result(SDL_SetWindowMouseGrab(window_, locked), WindowErrorCode::OperationFailed, "SDL3 set mouse grab failed."); !result) [[unlikely]] {
            return result;
        }
        if (auto result = sdl_bool_result(SDL_SetWindowRelativeMouseMode(window_, locked), WindowErrorCode::OperationFailed, "SDL3 set relative mouse mode failed."); !result) [[unlikely]] {
            return result;
        }
        if (auto result = sdl_bool_result(locked ? SDL_HideCursor() : SDL_ShowCursor(), WindowErrorCode::OperationFailed, "SDL3 set cursor visibility failed."); !result) [[unlikely]] {
            return result;
        }

        mouse_locked_ = locked;
        WindowEvent lock_event{locked ? WindowEventKind::MouseLocked : WindowEventKind::MouseUnlocked};
        lock_event.timestamp_ns = steady_now_ns();
        events_.push_back(lock_event);
        return {};
    }

    bool SDL3Window::mouse_locked() const noexcept {
        ZoneScopedN("SDL3Window::mouse_locked");
        auto lock = sdl_window_mutex().lock();
        return mouse_locked_;
    }

    WindowEffectResult SDL3Window::enable_window_effect(WindowEffect effect) noexcept {
        ZoneScopedN("SDL3Window::enable_window_effect");
        auto lock = sdl_window_mutex().lock();
        if (!window_) [[unlikely]] {
            Foundation::log_error("SDL3 enable native window effect rejected destroyed window: wrapper={} kind={}", static_cast<void *>(this), static_cast<i32>(effect.kind));
            return WindowEffectResult::failed("SDL3 window has already been destroyed.");
        }
        Foundation::log_info(
            "SDL3 enable native window effect: wrapper={} native_ptr={} id={} kind={} enabled={} color_argb=0x{:08X} linux_blur_protocol={}",
            static_cast<void *>(this),
            static_cast<void *>(window_),
            SDL_GetWindowID(window_),
            static_cast<i32>(effect.kind),
            effect.enabled,
            effect.color_argb,
            static_cast<i32>(effect.linux_blur_protocol));
        // native_window_handle_locked(), not native_window_handle(): this lock is already held
        // above, and sdl_window_mutex() is non-recursive — see its own doc comment.
        auto handle = native_window_handle_locked();
        if (!handle) [[unlikely]] {
            // SDL temporarily has no wl_surface while a Wayland window is hidden. Preserve the
            // requested state locally so disabling while hidden prevents show() from reapplying blur.
            if (effect.kind == WindowEffectKind::Blur && native_effect_handle_ &&
                native_effect_handle_->system == NativeWindowSystem::Wayland) {
                if (effect.enabled) {
                    active_blur_effect_ = effect;
                } else {
                    active_blur_effect_.reset();
                }
                return WindowEffectResult::success(
                    "Wayland blur state saved until SDL recreates the hidden window surface.");
            }
            return WindowEffectResult::failed("SDL3 native window handle is unavailable.");
        }

        if (effect.kind == WindowEffectKind::Blur) {
            // Keep the display/surface available for teardown even when protocol discovery later says
            // blur is unsupported; a hidden destructor otherwise cannot query SDL's null wl_surface.
            native_effect_handle_ = *handle;
        }
        WindowEffectResult result = enable_native_window_effect(*handle, effect);
        if (result.succeeded() && effect.kind == WindowEffectKind::Blur) {
            if (effect.enabled) {
                active_blur_effect_ = effect;
            } else {
                active_blur_effect_.reset();
            }
        }
        return result;
    }

    expected<void, WindowError> SDL3Window::set_effect(WindowEffect effect) noexcept {
        ZoneScopedN("SDL3Window::set_effect");
        return window_result_from_effect_result(enable_window_effect(effect));
    }

    expected<void, WindowError> SDL3Window::set_blur_enabled(bool enabled) noexcept {
        ZoneScopedN("SDL3Window::set_blur_enabled");
        return set_effect(WindowEffect::blur(enabled));
    }

    expected<void, WindowError> SDL3Window::set_transparent(bool enabled) noexcept {
        ZoneScopedN("SDL3Window::set_transparent");
        return set_effect(WindowEffect::transparent(enabled));
    }

    expected<vector<const char *>, WindowError>
    SDL3Window::required_vulkan_instance_extensions() const noexcept {
        ZoneScopedN("SDL3Window::required_vulkan_instance_extensions");
        u32 count = 0;
        const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!extensions) {
            return unexpected(sdl_error(WindowErrorCode::OperationFailed,
                                        "SDL_Vulkan_GetInstanceExtensions failed."));
        }
        try {
            return vector<const char *>{extensions, extensions + count};
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory,
                                          "Out of memory querying Vulkan WSI extensions."});
        }
    }

    expected<void, WindowError> SDL3Window::create_vulkan_surface(
        void *instance,
        const void *allocation_callbacks,
        void *surface_out) const noexcept {
        ZoneScopedN("SDL3Window::create_vulkan_surface");
        auto lock = sdl_window_mutex().lock();
        if (!window_ || instance == nullptr || surface_out == nullptr) {
            return unexpected(WindowError{
                WindowErrorCode::InvalidArgument,
                "SDL3 Vulkan surface creation requires a live window, instance, and output pointer.",
            });
        }
        if (!SDL_Vulkan_CreateSurface(
                window_,
                static_cast<VkInstance>(instance),
                static_cast<const VkAllocationCallbacks *>(allocation_callbacks),
                static_cast<VkSurfaceKHR *>(surface_out))) {
            return unexpected(sdl_error(WindowErrorCode::CreationFailed,
                                        "SDL_Vulkan_CreateSurface failed."));
        }
        return {};
    }

} // namespace SFT::Platform::Windowing::SDL3
