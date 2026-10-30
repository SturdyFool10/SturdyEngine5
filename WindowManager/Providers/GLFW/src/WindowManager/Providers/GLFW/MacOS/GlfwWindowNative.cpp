#include <Foundation/Foundation.hpp>

#include <expected>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <objc/message.h>
#include <objc/runtime.h>
#endif

#include <WindowManager/WindowLog.hpp>
#include <WindowManager/Providers/GLFW/GlfwWindowNative.hpp>

using std::expected;
using std::unexpected;

namespace SFT::WindowManager::GLFW::Detail {

    /// Returns the native window handle associated with this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    expected<NativeWindowHandle, WindowError> native_window_handle(void *window_handle) noexcept {
#if defined(__APPLE__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::WindowManager::Detail::window_error("GLFW Cocoa native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle requires a live window."});
        }

        NativeWindowHandle handle{NativeWindowSystem::Cocoa, nullptr, glfwGetCocoaWindow(window)};
        if (!handle.window) [[unlikely]] {
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Cocoa native handle is incomplete."});
        }
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Cocoa native handles are only available on Apple builds."});
#endif
    }

    /// No-op on macOS — WS_EX_NOREDIRECTIONBITMAP is a Win32/DWM concept with no Cocoa equivalent.
    /// See GlfwWindowNative.hpp's own doc comment and the Windows implementation
    /// (Platform/Windows/GlfwWindowNative.cpp) for why this exists at all.
    ///
    /// @param window_handle Unused.
    ///
    /// @note This function does not throw exceptions.
    void apply_composition_window_style(void *window_handle) noexcept { (void)window_handle; }

    /// No-op on macOS — matching SDL3's default window appearance is a Win32/DWM concept
    /// (DWMWA_USE_IMMERSIVE_DARK_MODE, WM_ERASEBKGND) with no Cocoa equivalent. See
    /// GlfwWindowNative.hpp's own doc comment and the Windows implementation
    /// (Platform/Windows/GlfwWindowNative.cpp) for why this exists at all.
    ///
    /// @param window_handle Unused.
    ///
    /// @note This function does not throw exceptions.
    void apply_windows_appearance(void *window_handle, bool paint_erase_background) noexcept {
        (void)window_handle;
        (void)paint_erase_background;
    }

    /// Performs the install ime composition hook operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param callback Callable invoked by the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept {
        (void)window_handle;
        (void)callback;
        (void)user_data;
        return false;
    }

    /// Removes the ime composition hook from its owning collection or system.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void remove_ime_composition_hook(void *window_handle) noexcept { (void)window_handle; }

    /// Sets the ime composition exclude rect for this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_composition_exclude_rect(void *window_handle, int x, int y, int width, int height) noexcept {
        (void)window_handle;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
    }

    /// Sets the ime enabled for this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_enabled(void *window_handle, bool enabled) noexcept {
        (void)window_handle;
        (void)enabled;
    }

#if defined(__APPLE__)
    namespace {
        // Driven through the Objective-C runtime so this stays a plain C++ translation unit. Written from Apple's
        // documentation without an Apple toolchain to run it on: treat as untested until it has run on a Mac.
        template <typename Result, typename... Args>
        Result send(id target, const char *selector, Args... args) noexcept {
            return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(target, sel_registerName(selector), args...);
        }
    } // namespace
#endif

    bool install_hit_test_hook(void *window_handle, const HitTestRegions *regions) noexcept {
        (void)regions;
        // Press-driven: a borderless NSWindow can be dragged by `performWindowDragWithEvent:`, but AppKit gives an
        // undecorated window no edge-resize request, so only drag regions work.
        return window_handle != nullptr;
    }

    void remove_hit_test_hook(void *window_handle) noexcept { (void)window_handle; }

    bool hit_test_is_press_driven(void *window_handle) noexcept { return window_handle != nullptr; }

    bool begin_interactive_drag(void *window_handle, HitTestResult edge) noexcept {
#if defined(__APPLE__)
        if (edge != HitTestResult::Draggable || window_handle == nullptr) {
            return false;
        }
        const id window = static_cast<id>(glfwGetCocoaWindow(static_cast<GLFWwindow *>(window_handle)));
        const id app = send<id>(reinterpret_cast<id>(objc_getClass("NSApplication")), "sharedApplication");
        // We are inside the press handler, called from the event dispatch, so this is the press itself.
        const id event = send<id>(app, "currentEvent");
        if (window == nullptr || event == nullptr) {
            return false;
        }
        send<void>(window, "performWindowDragWithEvent:", event);
        return true;
#else
        (void)window_handle;
        (void)edge;
        return false;
#endif
    }

    bool show_native_system_menu(void *window_handle, int x, int y) noexcept {
        (void)window_handle;
        (void)x;
        (void)y;
        return false; // macOS has no window-menu equivalent
    }

    bool set_native_taskbar_progress(void *window_handle, TaskbarProgress state, float value) noexcept {
        (void)window_handle;
        (void)state;
        (void)value;
        return false; // Dock progress needs a custom NSDockTile content view
    }

#if defined(__APPLE__)
    namespace {
        [[nodiscard]] id ns_string(std::string_view text) noexcept {
            return send<id>(reinterpret_cast<id>(objc_getClass("NSString")), "stringWithUTF8String:", std::string(text).c_str());
        }
        [[nodiscard]] id general_pasteboard() noexcept {
            return send<id>(reinterpret_cast<id>(objc_getClass("NSPasteboard")), "generalPasteboard");
        }
        /// MIME types map to plain-text pasteboard types so the system's own text type stays interoperable.
        [[nodiscard]] id pasteboard_type_for(std::string_view mime_type) noexcept {
            if (mime_type.rfind("text/plain", 0) == 0) {
                return ns_string("public.utf8-plain-text");
            }
            return ns_string(mime_type);
        }
    } // namespace
#endif

    bool native_typed_clipboard_available() noexcept { return true; }

    bool native_clipboard_has(std::string_view mime_type) noexcept {
#if defined(__APPLE__)
        const id types = send<id>(reinterpret_cast<id>(objc_getClass("NSArray")), "arrayWithObject:", pasteboard_type_for(mime_type));
        return send<id>(general_pasteboard(), "availableTypeFromArray:", types) != nullptr;
#else
        (void)mime_type;
        return false;
#endif
    }

    std::vector<std::byte> native_clipboard_get(std::string_view mime_type) noexcept {
        std::vector<std::byte> out;
#if defined(__APPLE__)
        const id data = send<id>(general_pasteboard(), "dataForType:", pasteboard_type_for(mime_type));
        if (data != nullptr) {
            const auto length = send<unsigned long>(data, "length");
            const auto *bytes = send<const std::byte *>(data, "bytes");
            if (bytes != nullptr) {
                out.assign(bytes, bytes + length);
            }
        }
#else
        (void)mime_type;
#endif
        return out;
    }

    bool native_clipboard_set(void *window_handle, std::string_view mime_type, std::span<const std::byte> bytes) noexcept {
        (void)window_handle;
#if defined(__APPLE__)
        const id board = general_pasteboard();
        const id type = pasteboard_type_for(mime_type);
        const id types = send<id>(reinterpret_cast<id>(objc_getClass("NSArray")), "arrayWithObject:", type);
        send<long>(board, "declareTypes:owner:", types, static_cast<id>(nullptr));
        const id data = send<id>(reinterpret_cast<id>(objc_getClass("NSData")), "dataWithBytes:length:", static_cast<const void *>(bytes.data()),
                                 static_cast<unsigned long>(bytes.size()));
        return send<signed char>(board, "setData:forType:", data, type) != 0;
#else
        (void)mime_type;
        (void)bytes;
        return false;
#endif
    }

    std::vector<std::string> native_clipboard_types() noexcept {
        std::vector<std::string> out;
#if defined(__APPLE__)
        const id types = send<id>(general_pasteboard(), "types");
        const auto count = send<unsigned long>(types, "count");
        for (unsigned long i = 0; i < count; ++i) {
            const id type = send<id>(types, "objectAtIndex:", i);
            if (const char *name = send<const char *>(type, "UTF8String")) {
                out.emplace_back(std::string_view{name} == "public.utf8-plain-text" ? "text/plain;charset=utf-8" : name);
            }
        }
#endif
        return out;
    }

    bool set_native_mouse_capture(void *window_handle, bool enabled) noexcept {
        (void)window_handle;
        (void)enabled;
        return false; // AppKit already routes drags to the window the press started in
    }

    bool set_native_cursor_confine(void *window_handle, const WindowRect *rect) noexcept {
        (void)window_handle;
        (void)rect;
        return false; // CGAssociateMouseAndMouseCursorPosition freezes the cursor; it cannot confine it to a rectangle
    }

    SystemTheme native_system_theme() noexcept {
#if defined(__APPLE__)
        const id defaults = send<id>(reinterpret_cast<id>(objc_getClass("NSUserDefaults")), "standardUserDefaults");
        const id key = send<id>(reinterpret_cast<id>(objc_getClass("NSString")), "stringWithUTF8String:", "AppleInterfaceStyle");
        const id value = send<id>(defaults, "stringForKey:", key);
        // The key exists (as "Dark") only while dark mode is on.
        return value != nullptr ? SystemTheme::Dark : SystemTheme::Light;
#else
        return SystemTheme::Unknown;
#endif
    }

} // namespace SFT::WindowManager::GLFW::Detail
