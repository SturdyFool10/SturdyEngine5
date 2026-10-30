#include <Foundation/Foundation.hpp>

#include <cstring>
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
#include <shobjidl.h>
#include <windowsx.h>
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

    namespace {

        constexpr UINT_PTR kHitTestSubclassId = 0xA17C07E5;
        constexpr UINT kWmPopupSystemMenu = 0x0313; // WM_POPUPSYSTEMMENU: undocumented but stable since Windows XP

        struct HitTestHookState {
            const HitTestRegions *regions = nullptr;
            /// An undecorated GLFW window is a plain WS_POPUP, which Windows will not size from the border. The hook
            /// adds WS_THICKFRAME (and removes it again) and zeroes the non-client area so the client still fills it.
            bool added_thick_frame = false;
        };

        LRESULT hit_test_code(HitTestResult result) noexcept {
            switch (result) {
                case HitTestResult::Normal: return HTCLIENT;
                case HitTestResult::Draggable: return HTCAPTION;
                case HitTestResult::ResizeTopLeft: return HTTOPLEFT;
                case HitTestResult::ResizeTop: return HTTOP;
                case HitTestResult::ResizeTopRight: return HTTOPRIGHT;
                case HitTestResult::ResizeRight: return HTRIGHT;
                case HitTestResult::ResizeBottomRight: return HTBOTTOMRIGHT;
                case HitTestResult::ResizeBottom: return HTBOTTOM;
                case HitTestResult::ResizeBottomLeft: return HTBOTTOMLEFT;
                case HitTestResult::ResizeLeft: return HTLEFT;
            }
            return HTCLIENT;
        }

        LRESULT CALLBACK hit_test_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
            auto *state = reinterpret_cast<HitTestHookState *>(dwRefData);
            if (state != nullptr && state->regions != nullptr && state->regions->enabled) {
                if (msg == WM_NCCALCSIZE && wParam == TRUE && state->added_thick_frame && !IsZoomed(hwnd)) {
                    return 0; // the client area is the whole window
                }
                if (msg == WM_NCHITTEST) {
                    POINT point{static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                                static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
                    RECT client{};
                    if (GetClientRect(hwnd, &client) && ScreenToClient(hwnd, &point)) {
                        const WindowExtent size{static_cast<u32>(client.right - client.left), static_cast<u32>(client.bottom - client.top)};
                        const HitTestResult result =
                            classify_hit(*state->regions, size, WindowPosition{point.x, point.y}, !IsZoomed(hwnd));
                        if (result != HitTestResult::Normal) {
                            return hit_test_code(result);
                        }
                    }
                }
            }
            if (msg == WM_NCDESTROY) {
                RemoveWindowSubclass(hwnd, hit_test_subclass_proc, kHitTestSubclassId);
                delete state;
            }
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

        [[nodiscard]] HWND hwnd_of(void *window_handle) noexcept {
            return window_handle != nullptr ? glfwGetWin32Window(static_cast<GLFWwindow *>(window_handle)) : nullptr;
        }

        // ITaskbarList3 / CLSID_TaskbarList, spelled out so no uuid header machinery is needed.
        const GUID kClsidTaskbarList = {0x56FDF344, 0xFD6D, 0x11d0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
        const GUID kIidTaskbarList3 = {0xEA1AFB91, 0x9E28, 0x4B86, {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};

    } // namespace

    bool install_hit_test_hook(void *window_handle, const HitTestRegions *regions) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr || regions == nullptr) {
            return false;
        }
        DWORD_PTR existing = 0;
        if (GetWindowSubclass(hwnd, hit_test_subclass_proc, kHitTestSubclassId, &existing)) {
            return true; // the hook reads the same `HitTestRegions` object live
        }
        auto *state = new HitTestHookState{regions, false};
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & (WS_CAPTION | WS_THICKFRAME)) == 0) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            state->added_thick_frame = true;
        }
        if (!SetWindowSubclass(hwnd, hit_test_subclass_proc, kHitTestSubclassId, reinterpret_cast<DWORD_PTR>(state))) {
            delete state;
            return false;
        }
        return true;
    }

    void remove_hit_test_hook(void *window_handle) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr) {
            return;
        }
        DWORD_PTR existing = 0;
        if (!GetWindowSubclass(hwnd, hit_test_subclass_proc, kHitTestSubclassId, &existing)) {
            return;
        }
        auto *state = reinterpret_cast<HitTestHookState *>(existing);
        RemoveWindowSubclass(hwnd, hit_test_subclass_proc, kHitTestSubclassId);
        if (state != nullptr && state->added_thick_frame) {
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_THICKFRAME));
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        delete state;
    }

    bool hit_test_is_press_driven(void *window_handle) noexcept {
        (void)window_handle;
        return false;
    }

    bool begin_interactive_drag(void *window_handle, HitTestResult edge) noexcept {
        (void)window_handle;
        (void)edge;
        return false;
    }

    bool show_native_system_menu(void *window_handle, int x, int y) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr) {
            return false;
        }
        POINT point{x, y};
        ClientToScreen(hwnd, &point);
        // Posted, not sent: the menu runs its own modal loop, which must not run inside the caller's window lock.
        return PostMessageW(hwnd, kWmPopupSystemMenu, 0, MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y))) != FALSE;
    }

    bool set_native_taskbar_progress(void *window_handle, TaskbarProgress state, float value) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr) {
            return false;
        }
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // RPC_E_CHANGED_MODE means this thread already has a COM apartment of the other kind, which is fine to use.
        const bool uninitialize = SUCCEEDED(initialized);
        ITaskbarList3 *taskbar = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(kClsidTaskbarList, nullptr, CLSCTX_INPROC_SERVER, kIidTaskbarList3,
                                       reinterpret_cast<void **>(&taskbar))) &&
            taskbar != nullptr) {
            if (SUCCEEDED(taskbar->HrInit())) {
                TBPFLAG flag = TBPF_NOPROGRESS;
                switch (state) {
                    case TaskbarProgress::None: flag = TBPF_NOPROGRESS; break;
                    case TaskbarProgress::Indeterminate: flag = TBPF_INDETERMINATE; break;
                    case TaskbarProgress::Normal: flag = TBPF_NORMAL; break;
                    case TaskbarProgress::Paused: flag = TBPF_PAUSED; break;
                    case TaskbarProgress::Error: flag = TBPF_ERROR; break;
                }
                ok = SUCCEEDED(taskbar->SetProgressState(hwnd, flag));
                if (ok && (flag == TBPF_NORMAL || flag == TBPF_PAUSED || flag == TBPF_ERROR)) {
                    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                    ok = SUCCEEDED(taskbar->SetProgressValue(hwnd, static_cast<ULONGLONG>(clamped * 1000.0f), 1000));
                }
            }
            taskbar->Release();
        }
        if (uninitialize) {
            CoUninitialize();
        }
        return ok;
    }

    namespace {

        // Windows names clipboard formats by registered string, so a MIME type is simply registered as one.
        [[nodiscard]] UINT clipboard_format_for(std::string_view mime_type) noexcept {
            return RegisterClipboardFormatA(std::string(mime_type).c_str());
        }

        /// Opens the clipboard, retrying briefly: another process may hold it for a moment.
        [[nodiscard]] bool open_clipboard_retrying(HWND owner) noexcept {
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (OpenClipboard(owner)) {
                    return true;
                }
                Sleep(5);
            }
            return false;
        }

    } // namespace

    bool native_typed_clipboard_available() noexcept { return true; }

    bool native_clipboard_has(std::string_view mime_type) noexcept {
        const UINT format = clipboard_format_for(mime_type);
        return format != 0 && IsClipboardFormatAvailable(format) != FALSE;
    }

    std::vector<std::byte> native_clipboard_get(std::string_view mime_type) noexcept {
        std::vector<std::byte> out;
        const UINT format = clipboard_format_for(mime_type);
        if (format == 0 || !open_clipboard_retrying(nullptr)) {
            return out;
        }
        if (HANDLE handle = GetClipboardData(format)) {
            if (const void *data = GlobalLock(handle)) {
                const SIZE_T size = GlobalSize(handle);
                out.assign(static_cast<const std::byte *>(data), static_cast<const std::byte *>(data) + size);
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return out;
    }

    bool native_clipboard_set(void *window_handle, std::string_view mime_type, std::span<const std::byte> bytes) noexcept {
        const UINT format = clipboard_format_for(mime_type);
        if (format == 0 || !open_clipboard_retrying(hwnd_of(window_handle))) {
            return false;
        }
        bool ok = false;
        if (EmptyClipboard()) {
            // The clipboard owns the block once SetClipboardData succeeds, so it is only freed here on failure.
            if (HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes.size() == 0 ? 1 : bytes.size())) {
                if (void *destination = GlobalLock(block)) {
                    std::memcpy(destination, bytes.data(), bytes.size());
                    GlobalUnlock(block);
                    ok = SetClipboardData(format, block) != nullptr;
                }
                if (!ok) {
                    GlobalFree(block);
                }
            }
        }
        CloseClipboard();
        return ok;
    }

    std::vector<std::string> native_clipboard_types() noexcept {
        std::vector<std::string> out;
        if (!open_clipboard_retrying(nullptr)) {
            return out;
        }
        for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
            if (format == CF_UNICODETEXT || format == CF_TEXT) {
                out.emplace_back("text/plain;charset=utf-8");
                continue;
            }
            char name[256]{};
            if (GetClipboardFormatNameA(format, name, static_cast<int>(sizeof(name))) > 0) {
                out.emplace_back(name);
            }
        }
        CloseClipboard();
        return out;
    }

    bool set_native_mouse_capture(void *window_handle, bool enabled) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr) {
            return false;
        }
        if (enabled) {
            SetCapture(hwnd);
            return true;
        }
        return ReleaseCapture() != FALSE;
    }

    bool set_native_cursor_confine(void *window_handle, const WindowRect *rect) noexcept {
        const HWND hwnd = hwnd_of(window_handle);
        if (hwnd == nullptr) {
            return false;
        }
        if (rect == nullptr) {
            return ClipCursor(nullptr) != FALSE;
        }
        // Window (client) coordinates -> screen coordinates, which is what ClipCursor takes.
        POINT top_left{rect->x, rect->y};
        POINT bottom_right{rect->x + rect->width, rect->y + rect->height};
        if (!ClientToScreen(hwnd, &top_left) || !ClientToScreen(hwnd, &bottom_right)) {
            return false;
        }
        const RECT screen{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
        return ClipCursor(&screen) != FALSE;
    }

    SystemTheme native_system_theme() noexcept {
        DWORD uses_light = 1;
        DWORD size = sizeof(uses_light);
        const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &uses_light, &size);
        if (status != ERROR_SUCCESS) {
            return SystemTheme::Unknown;
        }
        return uses_light != 0 ? SystemTheme::Light : SystemTheme::Dark;
    }

#else

    bool install_hit_test_hook(void *window_handle, const HitTestRegions *regions) noexcept {
        (void)window_handle;
        (void)regions;
        return false;
    }

    void remove_hit_test_hook(void *window_handle) noexcept { (void)window_handle; }

    bool hit_test_is_press_driven(void *window_handle) noexcept {
        (void)window_handle;
        return false;
    }

    bool begin_interactive_drag(void *window_handle, HitTestResult edge) noexcept {
        (void)window_handle;
        (void)edge;
        return false;
    }

    bool show_native_system_menu(void *window_handle, int x, int y) noexcept {
        (void)window_handle;
        (void)x;
        (void)y;
        return false;
    }

    bool set_native_taskbar_progress(void *window_handle, TaskbarProgress state, float value) noexcept {
        (void)window_handle;
        (void)state;
        (void)value;
        return false;
    }

    bool set_native_mouse_capture(void *window_handle, bool enabled) noexcept {
        (void)window_handle;
        (void)enabled;
        return false;
    }

    bool set_native_cursor_confine(void *window_handle, const WindowRect *rect) noexcept {
        (void)window_handle;
        (void)rect;
        return false;
    }

    bool native_typed_clipboard_available() noexcept { return false; }
    bool native_clipboard_has(std::string_view) noexcept { return false; }
    std::vector<std::byte> native_clipboard_get(std::string_view) noexcept { return {}; }
    bool native_clipboard_set(void *, std::string_view, std::span<const std::byte>) noexcept { return false; }
    std::vector<std::string> native_clipboard_types() noexcept { return {}; }

    SystemTheme native_system_theme() noexcept { return SystemTheme::Unknown; }

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
