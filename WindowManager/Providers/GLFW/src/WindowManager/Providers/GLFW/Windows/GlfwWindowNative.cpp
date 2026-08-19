#include <Foundation/Foundation.hpp>

#include <expected>
#include <vector>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#endif

#include <WindowManager/WindowLog.hpp>
#include <WindowManager/Providers/GLFW/GlfwWindowNative.hpp>

using std::expected;
using std::unexpected;
using std::vector;

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
#if defined(_WIN32)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::WindowManager::Detail::window_error("GLFW Win32 native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Win32 native handle requires a live window."});
        }

        NativeWindowHandle handle{NativeWindowSystem::Win32, nullptr, glfwGetWin32Window(window)};
        if (!handle.window) [[unlikely]] {
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Win32 native handle is incomplete."});
        }
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Win32 native handles are only available on Windows builds."});
#endif
    }

#if defined(_WIN32)

    /// Sets WS_EX_NOREDIRECTIONBITMAP on `window_handle`'s HWND.
    ///
    /// @param window_handle Opaque `GLFWwindow*` whose native HWND this style is applied to.
    ///
    /// @note DirectComposition's CreateTargetForHwnd composites its visual tree *over* the window's
    ///       own DWM redirection surface, not instead of it — and that surface is opaque. Without
    ///       WS_EX_NOREDIRECTIONBITMAP, a fully transparent (alpha=0) pixel in the composition visual
    ///       still shows the opaque redirection surface underneath rather than the desktop, which is
    ///       exactly "transparency doesn't change when the compositor alpha mode is toggled": the
    ///       compositor alpha mode was never the missing piece, the redirection surface always was.
    ///       Both graphics backends try DirectComposition for every swapchain generation they create
    ///       on a Vulkan/Direct3D window (see VulkanRhiBridgeSwapchain.cpp's create_swapchain() and
    ///       D3D12DeviceSwapchain.cpp's wants_transparency branch, both of which attempt
    ///       CreateSwapChainForComposition unconditionally), so this needs applying to every such
    ///       window, not only ones created with WindowConfig::transparent set.
    /// @note This style can only take effect if it is set *before* DWM ever allocates that surface
    ///       for the window, which happens the first time the window is mapped/shown — not at
    ///       glfwCreateWindow time. GLFW has no public API to request WS_EX_NOREDIRECTIONBITMAP at
    ///       window creation, so GLFWWindow::construct() forces the window hidden (GLFW_VISIBLE =
    ///       GLFW_FALSE) whenever it would otherwise be shown immediately, calls this right after
    ///       glfwCreateWindow returns, and only then shows it if the caller asked for a visible
    ///       window — the same sequencing SDL3Impl.cpp's identically-named
    ///       apply_composition_window_style() uses for the same reason.
    /// @note This function does not throw exceptions.
    void apply_composition_window_style(void *window_handle) noexcept {
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return;
        }
        const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_NOREDIRECTIONBITMAP);
    }

    namespace {

        // Same registry key Windows' own Settings app and every documented "does this user prefer
        // dark apps" sample reads — mirrors what SDL3's WIN_UpdateDarkModeForHWND ultimately falls
        // back to (ShouldAppsUseDarkMode(), an undocumented uxtheme.dll ordinal SDL3 prefers when
        // available) without depending on an undocumented ordinal ourselves. Missing key/value (very
        // old Windows builds predating this setting) defaults to light, matching that build's own
        // lack of a dark title-bar concept.
        [[nodiscard]] bool system_prefers_dark_mode() noexcept {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              0, KEY_READ, &key) != ERROR_SUCCESS) {
                return false;
            }
            DWORD value = 1; // AppsUseLightTheme: 1 = light (the documented default).
            DWORD size = sizeof(value);
            DWORD type = REG_DWORD;
            const LSTATUS status = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                                     reinterpret_cast<BYTE *>(&value), &size);
            RegCloseKey(key);
            return status == ERROR_SUCCESS && type == REG_DWORD && value == 0;
        }

        constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
        constexpr UINT_PTR kEraseBackgroundSubclassId = 0xE6A5EBC0;

        // Matches SDL3's own WM_ERASEBKGND handling (SDL_windowsevents.c): GLFW's window class
        // registers no background brush and its own WM_ERASEBKGND handler just returns TRUE without
        // painting anything (win32_window.c), so the client area shows whatever GDI/DWM's initial
        // surface content is — effectively white — until the first real present. Filling black here
        // instead makes that (usually sub-frame, but visible on a slow GPU/driver init) gap match
        // SDL3's dark appearance rather than standing out as a bright flash.
        //
        // Never installed on a WS_EX_NOREDIRECTIONBITMAP window (see apply_composition_window_style
        // and apply_windows_appearance's own `paint_erase_background` parameter below) — GetDC/FillRect
        // here forces GDI to allocate the very redirection surface WS_EX_NOREDIRECTIONBITMAP exists to
        // suppress, and that opaque surface then sits permanently behind DirectComposition's visual
        // tree, blocking a transparent swapchain exactly like the bleed-through
        // apply_composition_window_style's own doc comment describes — this GDI paint would recreate
        // the same class of problem for those windows, not fix it.
        LRESULT CALLBACK erase_background_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                         UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/) {
            if (msg == WM_ERASEBKGND) {
                RECT client_rect{};
                GetClientRect(hwnd, &client_rect);
                HDC dc = reinterpret_cast<HDC>(wParam);
                HBRUSH black_brush = CreateSolidBrush(RGB(0, 0, 0));
                FillRect(dc, &client_rect, black_brush);
                DeleteObject(black_brush);
                return 1;
            }
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

    } // namespace

    void apply_windows_appearance(void *window_handle, bool paint_erase_background) noexcept {
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return;
        }
        const BOOL dark = system_prefers_dark_mode() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
        if (paint_erase_background) {
            SetWindowSubclass(hwnd, erase_background_subclass_proc, kEraseBackgroundSubclassId, 0);
        }
    }

#else // !defined(_WIN32)

    void apply_composition_window_style(void *window_handle) noexcept { (void)window_handle; }
    void apply_windows_appearance(void *window_handle, bool paint_erase_background) noexcept {
        (void)window_handle;
        (void)paint_erase_background;
    }

#endif

#if defined(_WIN32)

    namespace {


        constexpr UINT_PTR kImeSubclassId = 0xF7E5D1C3;

        struct ImeSubclassState {
            ImePreeditCallback callback = nullptr;
            void *userData = nullptr;
            int rectX = 0;
            int rectY = 0;
            int rectWidth = 0;
            int rectHeight = 0;


            HIMC savedContext = nullptr;
        };

        /// Handles the ime subclass proc callback and updates the associated platform state.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param msg `msg` value used by the operation.
        /// @param wParam `wParam` value used by the operation.
        /// @param lParam `lParam` value used by the operation.
        /// @param uIdSubclass `uIdSubclass` value used by the operation.
        /// @param dwRefData `dwRefData` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LRESULT CALLBACK ime_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        /// Resolves the ime state associated with the supplied key, handle, or resource.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ImeSubclassState *ime_state_for(HWND hwnd) noexcept {
            DWORD_PTR refData = 0;
            if (!GetWindowSubclass(hwnd, ime_subclass_proc, kImeSubclassId, &refData)) {
                return nullptr;
            }
            return reinterpret_cast<ImeSubclassState *>(refData);
        }

        /// Handles the ime subclass proc callback and updates the associated platform state.
        ///
        /// @param hwnd `hwnd` value used by the operation.
        /// @param msg `msg` value used by the operation.
        /// @param wParam `wParam` value used by the operation.
        /// @param lParam `lParam` value used by the operation.
        /// @param dwRefData `dwRefData` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        LRESULT CALLBACK ime_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR                , DWORD_PTR dwRefData) {
            auto *state = reinterpret_cast<ImeSubclassState *>(dwRefData);

            if (msg == WM_IME_STARTCOMPOSITION) {


                if (HIMC himc = ImmGetContext(hwnd)) {
                    COMPOSITIONFORM cf{};
                    cf.dwStyle = CFS_EXCLUDE;
                    cf.ptCurrentPos.x = state->rectX;
                    cf.ptCurrentPos.y = state->rectY;
                    cf.rcArea.left = state->rectX;
                    cf.rcArea.top = state->rectY;
                    cf.rcArea.right = state->rectX + state->rectWidth;
                    cf.rcArea.bottom = state->rectY + state->rectHeight;
                    ImmSetCompositionWindow(himc, &cf);
                    ImmReleaseContext(hwnd, himc);
                }
            } else if (msg == WM_IME_COMPOSITION) {
                if ((lParam & GCS_COMPSTR) != 0 && state->callback != nullptr) {
                    if (HIMC himc = ImmGetContext(hwnd)) {


                        const LONG size = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
                        if (size > 0) {
                            vector<WCHAR> buffer((static_cast<usize>(size) / sizeof(WCHAR)) + 1, L'\0');
                            ImmGetCompositionStringW(himc, GCS_COMPSTR, buffer.data(),
                                                     static_cast<DWORD>(size));

                            const int utf8Size = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1,
                                                                     nullptr, 0, nullptr, nullptr);
                            if (utf8Size > 0) {
                                vector<char> utf8(static_cast<usize>(utf8Size), '\0');
                                WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1, utf8.data(), utf8Size,
                                                    nullptr, nullptr);


                                const int cursorPos = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
                                state->callback(utf8.data(), cursorPos, state->userData);
                            }
                        }
                        ImmReleaseContext(hwnd, himc);
                    }
                }
            } else if (msg == WM_IME_ENDCOMPOSITION) {


                if (state->callback != nullptr) {
                    state->callback("", -1, state->userData);
                }
            }

            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

    } // namespace

    /// Performs the install ime composition hook operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param callback Callable invoked by the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept {
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window || !callback) [[unlikely]] {
            return false;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return false;
        }

        auto *state = new ImeSubclassState{};
        state->callback = callback;
        state->userData = user_data;

        if (!SetWindowSubclass(hwnd, ime_subclass_proc, kImeSubclassId,
                               reinterpret_cast<DWORD_PTR>(state))) {
            delete state;
            return false;
        }
        return true;
    }

    /// Removes the ime composition hook from its owning collection or system.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void remove_ime_composition_hook(void *window_handle) noexcept {
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return;
        }

        ImeSubclassState *state = ime_state_for(hwnd);
        RemoveWindowSubclass(hwnd, ime_subclass_proc, kImeSubclassId);
        delete state;
    }

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
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return;
        }

        if (ImeSubclassState *state = ime_state_for(hwnd)) {
            state->rectX = x;
            state->rectY = y;
            state->rectWidth = width;
            state->rectHeight = height;
        }
    }

    /// Sets the ime enabled for this `Detail`.
    ///
    /// @param window_handle Window used or affected by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @note This function does not throw exceptions.
    void set_ime_enabled(void *window_handle, bool enabled) noexcept {
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }
        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) [[unlikely]] {
            return;
        }

        ImeSubclassState *state = ime_state_for(hwnd);
        if (!state) [[unlikely]] {
            return;
        }

        if (enabled) {


            if (state->savedContext) {
                ImmAssociateContext(hwnd, state->savedContext);
                state->savedContext = nullptr;
            }
        } else {
            if (!state->savedContext) {
                state->savedContext = ImmAssociateContext(hwnd, nullptr);
            }
        }
    }

#else

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

#endif

} // namespace SFT::WindowManager::GLFW::Detail
