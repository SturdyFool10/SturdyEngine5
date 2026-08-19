#pragma once

#include <expected>

#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowError.hpp>

namespace SFT::WindowManager::GLFW::Detail {

    /// Returns the requested native window handle.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::expected<NativeWindowHandle, WindowError>
    native_window_handle(void *window) noexcept;

    /// Windows only, no-op elsewhere: sets WS_EX_NOREDIRECTIONBITMAP on `window_handle`'s HWND.
    ///
    /// @param window_handle Opaque `GLFWwindow*` whose native HWND this style is applied to.
    ///
    /// @note See this function's own doc comment in GlfwWindowNative.cpp for why every Vulkan/
    ///       Direct3D window needs this applied before it is ever shown — mirrors SDL3Impl.cpp's
    ///       identically-named apply_composition_window_style() (the same fix, same reasoning, both
    ///       backends need it independently since neither wraps the other).
    /// @note This function does not throw exceptions.
    void apply_composition_window_style(void *window_handle) noexcept;

    /// Windows only, no-op elsewhere: makes `window_handle`'s HWND match SDL3's default window
    /// appearance instead of GLFW's own (a white title bar and a light-colored client area until the
    /// first real present, both because GLFW's Win32 backend does neither of the two things SDL3's
    /// does automatically).
    ///
    /// @param window_handle Opaque `GLFWwindow*` this appearance fix is applied to.
    /// @param paint_erase_background Whether to also install a WM_ERASEBKGND subclass that fills the
    ///        client area black until the first real paint (matching SDL3's own WM_ERASEBKGND
    ///        handling — see SDL_windowsevents.c). **Must be false** for a
    ///        WS_EX_NOREDIRECTIONBITMAP window (see apply_composition_window_style) — painting via
    ///        GetDC/FillRect there forces GDI to allocate the very redirection surface
    ///        WS_EX_NOREDIRECTIONBITMAP exists to suppress, permanently blocking a transparent
    ///        DirectComposition swapchain behind an opaque black surface.
    ///
    /// @note Always applies the current system light/dark theme preference via
    ///       DWMWA_USE_IMMERSIVE_DARK_MODE (matching SDL3's WIN_UpdateDarkModeForHWND), regardless of
    ///       `paint_erase_background`. Call this right after window creation, before the window is
    ///       ever shown, same timing requirement as apply_composition_window_style() above.
    /// @note This function does not throw exceptions.
    void apply_windows_appearance(void *window_handle, bool paint_erase_background) noexcept;

    /// Invoked whenever the platform's input method updates its in-progress composition (preedit)
    /// string.
    ///
    /// `utf8Text` is NUL-terminated; an empty string means composition has ended — matches
    /// WindowManager::WindowTextEditingEvent's own "empty = ended" contract exactly (see that
    /// struct's doc comment), so a caller wiring this into a WindowEvent needs no platform-specific
    /// special case. `cursorPos` is the composition string's own cursor position, or -1 when not
    /// applicable (composition ended, or the platform didn't report one). `userData` is whatever was
    /// passed to install_ime_composition_hook(), unchanged.
    using ImePreeditCallback = void (*)(const char *utf8Text, int cursorPos, void *userData);


    /// Performs the install ime composition hook operation using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param callback Callable invoked by the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept;


    /// Removes the ime composition hook from its owning collection or system.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void remove_ime_composition_hook(void *window_handle) noexcept;


    /// Sets the ime composition exclude rect from the supplied value.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param x `x` value used by the operation.
    /// @param y `y` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_composition_exclude_rect(void *window_handle, int x, int y, int width, int height) noexcept;


    /// Sets the ime enabled for this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_enabled(void *window_handle, bool enabled) noexcept;

} // namespace SFT::WindowManager::GLFW::Detail
