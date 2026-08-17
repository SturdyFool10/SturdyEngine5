#include <Foundation/src/Foundation.hpp>

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
#include <imm.h>
#endif

#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/GLFW/GlfwWindowNative.hpp>

using std::expected;
using std::unexpected;
using std::vector;

namespace SFT::Platform::Windowing::GLFW::Detail {

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
            ::SFT::Platform::Windowing::Detail::window_error("GLFW Win32 native handle rejected null window.");
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

} // namespace SFT::Platform::Windowing::GLFW::Detail
