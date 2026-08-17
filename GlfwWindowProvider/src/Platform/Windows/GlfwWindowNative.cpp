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

        // Keyed per-HWND by SetWindowSubclass's own (hwnd, subclass proc, id) tuple — a single
        // hardcoded id is fine (and how Microsoft's own SetWindowSubclass examples do it) since
        // uniqueness only needs to hold within one HWND for one given subclass proc, not globally.
        constexpr UINT_PTR kImeSubclassId = 0xF7E5D1C3;

        struct ImeSubclassState {
            ImePreeditCallback callback = nullptr;
            void *userData = nullptr;
            int rectX = 0;
            int rectY = 0;
            int rectWidth = 0;
            int rectHeight = 0;
            // Saved by set_ime_enabled(false) (ImmAssociateContext(hwnd, NULL)) so a later
            // set_ime_enabled(true) restores the exact context that was disassociated, rather than
            // assuming a fresh ImmGetContext() would return an equivalent one. NULL when IME is
            // currently enabled (the common case) or was never disabled.
            HIMC savedContext = nullptr;
        };

        LRESULT CALLBACK ime_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        [[nodiscard]] ImeSubclassState *ime_state_for(HWND hwnd) noexcept {
            DWORD_PTR refData = 0;
            if (!GetWindowSubclass(hwnd, ime_subclass_proc, kImeSubclassId, &refData)) {
                return nullptr;
            }
            return reinterpret_cast<ImeSubclassState *>(refData);
        }

        LRESULT CALLBACK ime_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
            auto *state = reinterpret_cast<ImeSubclassState *>(dwRefData);

            if (msg == WM_IME_STARTCOMPOSITION) {
                // Excludes (only) the application's own composition-rendering rect from the
                // platform's default composition window — does not consume the message, so the
                // candidate-list window (this engine has no UI of its own for it) keeps using the
                // platform's own default positioning/drawing.
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
                        // Query-length-then-fetch: a first call with a NULL/zero buffer returns the
                        // required size in bytes (of UTF-16 code units), not a character count.
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

                                // GCS_CURSORPOS reports the cursor position as the call's own return
                                // value (a character offset into the composition string), not
                                // through an output buffer — a second, independent call, not a
                                // continuation of the GCS_COMPSTR read above.
                                const int cursorPos = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
                                state->callback(utf8.data(), cursorPos, state->userData);
                            }
                        }
                        ImmReleaseContext(hwnd, himc);
                    }
                }
            } else if (msg == WM_IME_ENDCOMPOSITION) {
                // Empty text + cursorPos -1 is this codebase's own "composition ended" contract —
                // see ImePreeditCallback's own doc comment (GlfwWindowNative.hpp).
                if (state->callback != nullptr) {
                    state->callback("", -1, state->userData);
                }
            }

            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

    } // namespace

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
            // Only restore if a context was actually saved by a prior disable — disabling twice in
            // a row without an enable between them must not clobber the originally-saved context.
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

#else // !defined(_WIN32)

    bool install_ime_composition_hook(void *window_handle, ImePreeditCallback callback, void *user_data) noexcept {
        (void)window_handle;
        (void)callback;
        (void)user_data;
        return false;
    }

    void remove_ime_composition_hook(void *window_handle) noexcept { (void)window_handle; }

    void set_ime_composition_exclude_rect(void *window_handle, int x, int y, int width, int height) noexcept {
        (void)window_handle;
        (void)x;
        (void)y;
        (void)width;
        (void)height;
    }

    void set_ime_enabled(void *window_handle, bool enabled) noexcept {
        (void)window_handle;
        (void)enabled;
    }

#endif

} // namespace SFT::Platform::Windowing::GLFW::Detail
