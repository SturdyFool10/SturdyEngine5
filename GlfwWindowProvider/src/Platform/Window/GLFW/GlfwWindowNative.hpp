#pragma once

#include <expected>

#include <Platform/Window/WindowConfig.hpp>
#include <Platform/Window/WindowError.hpp>

namespace SFT::Platform::Windowing::GLFW::Detail {

    [[nodiscard]] std::expected<NativeWindowHandle, WindowError>
    native_window_handle(void *window) noexcept;

    // Invoked whenever the platform's input method updates its in-progress composition (preedit)
    // string. `utf8Text` is NUL-terminated; an empty string means composition has ended — matches
    // Platform::Windowing::WindowTextEditingEvent's own "empty = ended" contract exactly (see that
    // struct's doc comment), so a caller wiring this into a WindowEvent needs no platform-specific
    // special case. `cursorPos` is the composition string's own cursor position, or -1 when not
    // applicable (composition ended, or the platform didn't report one). `userData` is whatever was
    // passed to install_ime_composition_hook(), unchanged.
    using ImePreeditCallback = void (*)(const char *utf8Text, int cursorPos, void *userData);

    // Installs a native IME composition hook for `window_handle` (opaque, same contract as
    // native_window_handle()'s own parameter — a live GLFWwindow*), independent of GLFW's own event
    // handling: GLFW never needs to know this exists, since it never patches or intercepts GLFW's
    // own window procedure/event loop, only adds a platform-native listener alongside it (a Win32
    // window-message subclass, an X11 XIM preedit-callback input context, or an independently-bound
    // Wayland text-input-v3 object, depending on the platform file actually compiled in — see each
    // platform's own GlfwWindowNative.cpp). Returns false, doing nothing further, on a platform this
    // isn't implemented for yet; the caller treats that exactly like "this window happens to never
    // receive composition events," not an error condition, so nothing above this interface — not
    // GLFWWindow, not Engine, not UI — ever needs to know which platform (or whether any) is active.
    [[nodiscard]] bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept;

    // Reverses install_ime_composition_hook() — must be called before `window_handle` is destroyed.
    // A no-op if no hook was ever successfully installed (including on a platform where
    // install_ime_composition_hook() returned false).
    void remove_ime_composition_hook(void *window_handle) noexcept;

    // The window-client-area pixel rectangle (same coordinate space native_window_handle()'s own
    // caller already works in) the platform's native composition/candidate UI should avoid drawing
    // over — typically the focused text field's own bounds. No-op where IME support isn't
    // implemented. Takes effect the next time composition starts, not retroactively.
    void set_ime_composition_exclude_rect(void *window_handle, int x, int y, int width, int height) noexcept;

    // Enables or disables routing keystrokes through the platform's input method for this window at
    // all (as opposed to raw keystrokes bypassing it entirely) — the native-layer counterpart of
    // Platform::Windowing::Window::start_text_input()/stop_text_input(). No-op where IME support
    // isn't implemented.
    void set_ime_enabled(void *window_handle, bool enabled) noexcept;

} // namespace SFT::Platform::Windowing::GLFW::Detail
