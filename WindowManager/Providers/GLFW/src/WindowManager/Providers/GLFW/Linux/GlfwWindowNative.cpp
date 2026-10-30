#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)
#include <wayland-client.h>
#include "text-input-unstable-v3-client-protocol.h"
// The header above only *declares* zwp_text_input_v3_interface and its manager as extern; the
// wayland-scanner "private-code" output is what defines them. It has to be included in exactly one
// translation unit, and this is the only one that touches the protocol at all. Without it the
// build compiles cleanly and then fails at link with undefined zwp_text_input_*_interface, which
// is what happened -- every executable linking the GLFW provider failed to link, so only stale
// binaries ran.
#include "text-input-unstable-v3-client-protocol-code.h"
#endif

#if defined(Success)
#undef Success
#endif
#if defined(None)
#undef None
#endif
#if defined(Always)
#undef Always
#endif
#if defined(Bool)
#undef Bool
#endif
#endif

#include <WindowManager/WindowLog.hpp>
#include <WindowManager/Providers/GLFW/GlfwWindowNative.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::unexpected;
using std::uintptr_t;
using std::unordered_map;
using std::vector;
using std::wstring;

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
        ZoneScopedN("Windowing::GLFW::Detail::native_window_handle");
#if defined(__linux__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::WindowManager::Detail::window_error("GLFW Linux native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Linux native handle requires a live window."});
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            NativeWindowHandle handle{NativeWindowSystem::Wayland, glfwGetWaylandDisplay(), glfwGetWaylandWindow(window)};
            if (!handle.display || !handle.window) [[unlikely]] {
                ::SFT::WindowManager::Detail::window_error("GLFW Linux Wayland native handle missing display or surface: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Wayland native handle is incomplete."});
            }
            return handle;
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            NativeWindowHandle handle{
                NativeWindowSystem::X11,
                glfwGetX11Display(),
                reinterpret_cast<void *>(static_cast<uintptr_t>(glfwGetX11Window(window))),
            };
            if (!handle.display || !handle.window) [[unlikely]] {
                ::SFT::WindowManager::Detail::window_error("GLFW Linux X11 native handle missing display or window: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
                return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW X11 native handle is incomplete."});
            }
            return handle;
        }

        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Linux native handle platform is unsupported."});
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "GLFW Linux native handles are only available on Linux builds."});
#endif
    }

    /// No-op on Linux — WS_EX_NOREDIRECTIONBITMAP is a Win32/DWM concept with no X11/Wayland
    /// equivalent. See GlfwWindowNative.hpp's own doc comment and the Windows implementation
    /// (Platform/Windows/GlfwWindowNative.cpp) for why this exists at all.
    ///
    /// @param window_handle Unused.
    ///
    /// @note This function does not throw exceptions.
    void apply_composition_window_style(void *window_handle) noexcept { (void)window_handle; }

    /// No-op on Linux — matching SDL3's default window appearance is a Win32/DWM concept
    /// (DWMWA_USE_IMMERSIVE_DARK_MODE, WM_ERASEBKGND) with no X11/Wayland equivalent. See
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

#if defined(__linux__)

    namespace {


        struct X11ImeState {
            XIC ic = nullptr;
            ImePreeditCallback callback = nullptr;
            void *userData = nullptr;


            wstring preeditBuffer;
        };

        /// Returns the current or globally available x11 shared im value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] XIM &x11_shared_im() noexcept {


            static XIM im = nullptr;
            return im;
        }

        /// Returns the current or globally available x11 ime states value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] unordered_map<::Window, X11ImeState *> &x11_ime_states() noexcept {
            static unordered_map<::Window, X11ImeState *> states;
            return states;
        }

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param out `out` value used by the operation.
        /// @param codepoint `codepoint` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void append_utf8(std::string &out, char32_t codepoint) noexcept {
            if (codepoint <= 0x7F) {
                out.push_back(static_cast<char>(codepoint));
            } else if (codepoint <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            } else if (codepoint <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }

        /// Performs the wstring to UTF-8 operation for `Detail` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string wstring_to_utf8(const wstring &text) noexcept {
            std::string result;
            result.reserve(text.size() * 2);
            for (wchar_t ch : text) {
                append_utf8(result, static_cast<char32_t>(static_cast<uint32_t>(ch)));
            }
            return result;
        }


        /// Handles the x11 preedit start callback callback and updates the associated platform state.
        ///
        /// @param clientData `clientData` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        int x11_preedit_start_callback(XIC       , XPointer clientData, XPointer             ) {
            if (auto *state = reinterpret_cast<X11ImeState *>(clientData)) {
                state->preeditBuffer.clear();
            }
            return -1;
        }


        /// Handles the x11 preedit draw callback callback and updates the associated platform state.
        ///
        /// @param clientData `clientData` value used by the operation.
        /// @param callDataRaw `callDataRaw` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void x11_preedit_draw_callback(XIC       , XPointer clientData, XPointer callDataRaw) {
            auto *state = reinterpret_cast<X11ImeState *>(clientData);
            auto *callData = reinterpret_cast<XIMPreeditDrawCallbackStruct *>(callDataRaw);
            if (!state || !callData) [[unlikely]] {
                return;
            }

            wstring &buffer = state->preeditBuffer;
            const usize first = static_cast<usize>(std::max(callData->chg_first, 0));
            if (first <= buffer.size()) {
                const usize removeCount = static_cast<usize>(std::max(callData->chg_length, 0));
                buffer.erase(first, std::min(removeCount, buffer.size() - first));

                if (callData->text != nullptr) {
                    XIMText *text = callData->text;
                    wstring insertion;
                    insertion.reserve(static_cast<usize>(std::max(text->length, static_cast<unsigned short>(0))));
                    if (text->encoding_is_wchar) {
                        for (int i = 0; i < text->length; ++i) {
                            insertion.push_back(static_cast<wchar_t>(text->string.wide_char[i]));
                        }
                    } else if (text->string.multi_byte != nullptr) {


                        mbstate_t mbState{};
                        const char *cursor = text->string.multi_byte;
                        usize remaining = strlen(cursor);
                        while (remaining > 0) {
                            wchar_t wc = 0;
                            const usize consumed = mbrtowc(&wc, cursor, remaining, &mbState);
                            if (consumed == static_cast<usize>(-1) || consumed == static_cast<usize>(-2) || consumed == 0) {
                                break;
                            }
                            insertion.push_back(wc);
                            cursor += consumed;
                            remaining -= consumed;
                        }
                    }
                    buffer.insert(first, insertion);
                }
            }

            if (state->callback) {
                const std::string utf8 = wstring_to_utf8(buffer);
                state->callback(utf8.c_str(), callData->caret, state->userData);
            }
        }


        /// Handles the x11 preedit done callback callback and updates the associated platform state.
        ///
        /// @param clientData `clientData` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void x11_preedit_done_callback(XIC       , XPointer clientData, XPointer             ) {
            if (auto *state = reinterpret_cast<X11ImeState *>(clientData)) {
                state->preeditBuffer.clear();
                if (state->callback) {
                    state->callback("", -1, state->userData);
                }
            }
        }

        /// Performs the x11 install ime operation for `Detail` using the supplied arguments.
        ///
        /// @param xwindow Window used or affected by the operation.
        /// @param callback Callable invoked by the operation.
        /// @param userData `userData` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool x11_install_ime(::Window xwindow, ImePreeditCallback callback, void *userData) noexcept {
            Display *display = glfwGetX11Display();
            if (!display) [[unlikely]] {
                return false;
            }
            if (!x11_shared_im()) {


                x11_shared_im() = XOpenIM(display, nullptr, nullptr, nullptr);
                if (!x11_shared_im()) [[unlikely]] {


                    return false;
                }
            }

            auto *state = new X11ImeState{};
            state->callback = callback;
            state->userData = userData;

            XIMCallback startCb{reinterpret_cast<XPointer>(state), reinterpret_cast<XIMProc>(&x11_preedit_start_callback)};
            XIMCallback drawCb{reinterpret_cast<XPointer>(state), reinterpret_cast<XIMProc>(&x11_preedit_draw_callback)};
            XIMCallback doneCb{reinterpret_cast<XPointer>(state), reinterpret_cast<XIMProc>(&x11_preedit_done_callback)};

            XVaNestedList preeditList = XVaCreateNestedList(0,
                XNPreeditStartCallback, &startCb,
                XNPreeditDrawCallback, &drawCb,
                XNPreeditDoneCallback, &doneCb,
                nullptr);
            if (!preeditList) [[unlikely]] {
                delete state;
                return false;
            }

            XIC ic = XCreateIC(x11_shared_im(),
                XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                XNClientWindow, xwindow,
                XNFocusWindow, xwindow,
                XNPreeditAttributes, preeditList,
                nullptr);
            XFree(preeditList);

            if (!ic) [[unlikely]] {
                delete state;
                return false;
            }

            state->ic = ic;
            x11_ime_states()[xwindow] = state;
            return true;
        }

        /// Performs the x11 remove ime operation for `Detail` using the supplied arguments.
        ///
        /// @param xwindow Window used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void x11_remove_ime(::Window xwindow) noexcept {
            auto it = x11_ime_states().find(xwindow);
            if (it == x11_ime_states().end()) {
                return;
            }
            if (it->second->ic) {
                XDestroyIC(it->second->ic);
            }
            delete it->second;
            x11_ime_states().erase(it);
        }

        /// Performs the x11 set exclude rect operation for `Detail` using the supplied arguments.
        ///
        /// @param xwindow Window used or affected by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void x11_set_exclude_rect(::Window xwindow, int x, int y) noexcept {
            auto it = x11_ime_states().find(xwindow);
            if (it == x11_ime_states().end() || !it->second->ic) {
                return;
            }


            XPoint spot{static_cast<short>(x), static_cast<short>(y)};
            if (XVaNestedList list = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr)) {
                XSetICValues(it->second->ic, XNPreeditAttributes, list, nullptr);
                XFree(list);
            }
        }

        /// Performs the x11 set enabled operation for `Detail` using the supplied arguments.
        ///
        /// @param xwindow Window used or affected by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void x11_set_enabled(::Window xwindow, bool enabled) noexcept {
            auto it = x11_ime_states().find(xwindow);
            if (it == x11_ime_states().end() || !it->second->ic) {
                return;
            }
            if (enabled) {
                XSetICFocus(it->second->ic);
            } else {
                XUnsetICFocus(it->second->ic);
            }
        }

    } // namespace

#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)

    namespace {


        struct WaylandGlobals {
            wl_registry *registry = nullptr;
            wl_seat *seat = nullptr;
            zwp_text_input_manager_v3 *textInputManager = nullptr;
        };

        /// Returns the current or globally available wayland globals value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WaylandGlobals &wayland_globals() noexcept {
            static WaylandGlobals globals;
            return globals;
        }

        /// Handles the wayland registry global callback and updates the associated platform state.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param registry `registry` value used by the operation.
        /// @param name Name used to identify or label the target.
        /// @param interface `interface` value used by the operation.
        /// @param version `version` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_registry_global(void *data, wl_registry *registry, uint32_t name,
                                     const char *interface, uint32_t version) {
            auto *globals = static_cast<WaylandGlobals *>(data);
            if (std::strcmp(interface, wl_seat_interface.name) == 0 && !globals->seat) {
                globals->seat = static_cast<wl_seat *>(
                    wl_registry_bind(registry, name, &wl_seat_interface, std::min<uint32_t>(version, 8)));
            } else if (std::strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0 && !globals->textInputManager) {
                globals->textInputManager = static_cast<zwp_text_input_manager_v3 *>(
                    wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
            }
        }

        /// Handles the wayland registry global remove callback and updates the associated platform state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_registry_global_remove(void *         , wl_registry *             , uint32_t         ) {}

        constexpr wl_registry_listener kRegistryListener{
            .global = wayland_registry_global,
            .global_remove = wayland_registry_global_remove,
        };

        /// Returns the current or globally available wayland ensure globals value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wayland_ensure_globals() noexcept {
            WaylandGlobals &globals = wayland_globals();
            if (globals.seat && globals.textInputManager) {
                return true;
            }
            wl_display *display = glfwGetWaylandDisplay();
            if (!display) [[unlikely]] {
                return false;
            }
            if (!globals.registry) {
                globals.registry = wl_display_get_registry(display);
                if (!globals.registry) [[unlikely]] {
                    return false;
                }
                wl_registry_add_listener(globals.registry, &kRegistryListener, &globals);
            }
            wl_display_roundtrip(display);
            return globals.seat && globals.textInputManager;
        }

        struct WaylandImeState {
            zwp_text_input_v3 *textInput = nullptr;
            ImePreeditCallback callback = nullptr;
            void *userData = nullptr;


            std::string pendingPreedit;
            bool pendingPreeditSet = false;
        };

        /// Returns the current or globally available wayland ime states value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] unordered_map<wl_surface *, WaylandImeState *> &wayland_ime_states() noexcept {
            static unordered_map<wl_surface *, WaylandImeState *> states;
            return states;
        }

        /// Handles the wayland text input enter callback and updates the associated platform state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_enter(void *, zwp_text_input_v3 *, wl_surface *) {


        }

        /// Handles the wayland text input leave callback and updates the associated platform state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_leave(void *, zwp_text_input_v3 *, wl_surface *) {}

        /// Handles the wayland text input preedit string callback and updates the associated platform state.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param text Text consumed by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_preedit_string(void *data, zwp_text_input_v3 *, const char *text,
                                               int32_t                , int32_t              ) {
            auto *state = static_cast<WaylandImeState *>(data);
            if (!state) [[unlikely]] {
                return;
            }
            state->pendingPreedit = text ? text : "";
            state->pendingPreeditSet = true;
        }

        /// Handles the wayland text input commit string callback and updates the associated platform state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_commit_string(void *         , zwp_text_input_v3 *              , const char *         ) {


        }

        /// Handles the wayland text input delete surrounding text callback and updates the associated platform state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_delete_surrounding_text(void *, zwp_text_input_v3 *, uint32_t, uint32_t) {}

        /// Handles the wayland text input done callback and updates the associated platform state.
        ///
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void wayland_text_input_done(void *data, zwp_text_input_v3 *, uint32_t           ) {
            auto *state = static_cast<WaylandImeState *>(data);
            if (!state || !state->pendingPreeditSet || !state->callback) [[unlikely]] {
                return;
            }


            state->callback(state->pendingPreedit.c_str(), state->pendingPreedit.empty() ? -1 : 0, state->userData);
            state->pendingPreeditSet = false;
        }

        constexpr zwp_text_input_v3_listener kTextInputListener{
            .enter = wayland_text_input_enter,
            .leave = wayland_text_input_leave,
            .preedit_string = wayland_text_input_preedit_string,
            .commit_string = wayland_text_input_commit_string,
            .delete_surrounding_text = wayland_text_input_delete_surrounding_text,
            .done = wayland_text_input_done,
        };

        /// Performs the wayland install ime operation for `Detail` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param callback Callable invoked by the operation.
        /// @param userData `userData` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wayland_install_ime(wl_surface *surface, ImePreeditCallback callback, void *userData) noexcept {
            if (!wayland_ensure_globals()) {
                return false;
            }
            WaylandGlobals &globals = wayland_globals();

            zwp_text_input_v3 *textInput = zwp_text_input_manager_v3_get_text_input(globals.textInputManager, globals.seat);
            if (!textInput) [[unlikely]] {
                return false;
            }

            auto *state = new WaylandImeState{};
            state->textInput = textInput;
            state->callback = callback;
            state->userData = userData;

            zwp_text_input_v3_add_listener(textInput, &kTextInputListener, state);
            wayland_ime_states()[surface] = state;
            return true;
        }

        /// Performs the wayland remove ime operation for `Detail` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void wayland_remove_ime(wl_surface *surface) noexcept {
            auto it = wayland_ime_states().find(surface);
            if (it == wayland_ime_states().end()) {
                return;
            }
            if (it->second->textInput) {
                zwp_text_input_v3_destroy(it->second->textInput);
            }
            delete it->second;
            wayland_ime_states().erase(it);
        }

        /// Performs the wayland set exclude rect operation for `Detail` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param x `x` value used by the operation.
        /// @param y `y` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @note This function does not throw exceptions.
        void wayland_set_exclude_rect(wl_surface *surface, int x, int y, int width, int height) noexcept {
            auto it = wayland_ime_states().find(surface);
            if (it == wayland_ime_states().end() || !it->second->textInput) {
                return;
            }


            zwp_text_input_v3_set_cursor_rectangle(it->second->textInput, x, y, width, height);
            zwp_text_input_v3_commit(it->second->textInput);
        }

        /// Performs the wayland set enabled operation for `Detail` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function does not throw exceptions.
        void wayland_set_enabled(wl_surface *surface, bool enabled) noexcept {
            auto it = wayland_ime_states().find(surface);
            if (it == wayland_ime_states().end() || !it->second->textInput) {
                return;
            }
            if (enabled) {
                zwp_text_input_v3_enable(it->second->textInput);
            } else {
                zwp_text_input_v3_disable(it->second->textInput);
            }


            zwp_text_input_v3_commit(it->second->textInput);
        }

    } // namespace

#endif // defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)

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

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            return x11_install_ime(glfwGetX11Window(window), callback, user_data);
        }
#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            return wayland_install_ime(glfwGetWaylandWindow(window), callback, user_data);
        }
#endif
        return false;
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

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            x11_remove_ime(glfwGetX11Window(window));
            return;
        }
#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            wayland_remove_ime(glfwGetWaylandWindow(window));
        }
#endif
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

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            x11_set_exclude_rect(glfwGetX11Window(window), x, y);
            return;
        }
#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            wayland_set_exclude_rect(glfwGetWaylandWindow(window), x, y, width, height);
            return;
        }
#endif
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
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            return;
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_X11) {
            x11_set_enabled(glfwGetX11Window(window), enabled);
            return;
        }
#if defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            wayland_set_enabled(glfwGetWaylandWindow(window), enabled);
        }
#endif
    }

    namespace {

        [[nodiscard]] bool is_x11_window(void *window_handle) noexcept {
            return window_handle != nullptr && glfwGetPlatform() == GLFW_PLATFORM_X11 && glfwGetX11Display() != nullptr;
        }

        /// Sends a root-window client message the way EWMH asks window-manager requests to be sent.
        void send_wm_client_message(void *window_handle, const char *type, long d0, long d1, long d2, long d3, long d4) noexcept {
            auto *display = static_cast<Display *>(glfwGetX11Display());
            const ::Window window = glfwGetX11Window(static_cast<GLFWwindow *>(window_handle));
            XEvent event{};
            event.xclient.type = ClientMessage;
            event.xclient.window = window;
            event.xclient.message_type = XInternAtom(display, type, False);
            event.xclient.format = 32;
            event.xclient.data.l[0] = d0;
            event.xclient.data.l[1] = d1;
            event.xclient.data.l[2] = d2;
            event.xclient.data.l[3] = d3;
            event.xclient.data.l[4] = d4;
            XSendEvent(display, DefaultRootWindow(display), False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
            XFlush(display);
        }

        [[nodiscard]] std::string read_text_file(const std::string &path) noexcept {
            std::string text;
            if (FILE *file = std::fopen(path.c_str(), "rb")) {
                char buffer[4096];
                while (const size_t read = std::fread(buffer, 1, sizeof(buffer), file)) {
                    text.append(buffer, read);
                    if (text.size() > (1u << 20)) {
                        break;
                    }
                }
                std::fclose(file);
            }
            return text;
        }

        [[nodiscard]] std::string lowercase(std::string text) noexcept {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        /// The value of `key=` on its own line in an ini-style file, or empty.
        [[nodiscard]] std::string ini_value(const std::string &text, const char *key) noexcept {
            const std::string needle = std::string(key) + "=";
            size_t at = 0;
            while ((at = text.find(needle, at)) != std::string::npos) {
                if (at == 0 || text[at - 1] == '\n') {
                    const size_t start = at + needle.size();
                    const size_t end = text.find_first_of("\r\n", start);
                    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                }
                at += needle.size();
            }
            return {};
        }

    } // namespace

    bool install_hit_test_hook(void *window_handle, const HitTestRegions *regions) noexcept {
        (void)regions;
        // X11 is press-driven (the provider calls begin_interactive_drag from its mouse-button callback). GLFW does not
        // expose a Wayland window's xdg_toplevel, which is what a compositor-side move or resize request needs.
        return is_x11_window(window_handle);
    }

    void remove_hit_test_hook(void *window_handle) noexcept { (void)window_handle; }

    bool hit_test_is_press_driven(void *window_handle) noexcept { return is_x11_window(window_handle); }

    bool begin_interactive_drag(void *window_handle, HitTestResult edge) noexcept {
        if (!is_x11_window(window_handle)) {
            return false;
        }
        // _NET_WM_MOVERESIZE direction codes (EWMH).
        long direction = 8; // _NET_WM_MOVERESIZE_MOVE
        switch (edge) {
            case HitTestResult::Normal: return false;
            case HitTestResult::Draggable: direction = 8; break;
            case HitTestResult::ResizeTopLeft: direction = 0; break;
            case HitTestResult::ResizeTop: direction = 1; break;
            case HitTestResult::ResizeTopRight: direction = 2; break;
            case HitTestResult::ResizeRight: direction = 3; break;
            case HitTestResult::ResizeBottomRight: direction = 4; break;
            case HitTestResult::ResizeBottom: direction = 5; break;
            case HitTestResult::ResizeBottomLeft: direction = 6; break;
            case HitTestResult::ResizeLeft: direction = 7; break;
        }
        auto *display = static_cast<Display *>(glfwGetX11Display());
        ::Window root_return = 0;
        ::Window child_return = 0;
        int root_x = 0;
        int root_y = 0;
        int window_x = 0;
        int window_y = 0;
        unsigned int mask = 0;
        XQueryPointer(display, DefaultRootWindow(display), &root_return, &child_return, &root_x, &root_y, &window_x, &window_y, &mask);
        // The window manager takes over the pointer; release the implicit grab the press started.
        XUngrabPointer(display, CurrentTime);
        send_wm_client_message(window_handle, "_NET_WM_MOVERESIZE", root_x, root_y, direction, 1 /* Button1 */, 1 /* normal application */);
        return true;
    }

    bool show_native_system_menu(void *window_handle, int x, int y) noexcept {
        if (!is_x11_window(window_handle)) {
            return false;
        }
        auto *display = static_cast<Display *>(glfwGetX11Display());
        const ::Window window = glfwGetX11Window(static_cast<GLFWwindow *>(window_handle));
        ::Window child = 0;
        int root_x = 0;
        int root_y = 0;
        XTranslateCoordinates(display, window, DefaultRootWindow(display), x, y, &root_x, &root_y, &child);
        send_wm_client_message(window_handle, "_GTK_SHOW_WINDOW_MENU", 0 /* device */, root_x, root_y, 0, 0);
        return true;
    }

    bool set_native_taskbar_progress(void *window_handle, TaskbarProgress state, float value) noexcept {
        (void)window_handle;
        (void)state;
        (void)value;
        return false; // the Unity launcher API needs a session-bus connection; no X11/Wayland-level equivalent
    }

    bool set_native_cursor_confine(void *window_handle, const WindowRect *rect) noexcept {
        if (!is_x11_window(window_handle)) {
            return false; // Wayland pointer-constraints need the wl_pointer GLFW keeps to itself
        }
        auto *display = static_cast<Display *>(glfwGetX11Display());
        const ::Window window = glfwGetX11Window(static_cast<GLFWwindow *>(window_handle));
        // The window each confinement uses: XGrabPointer can confine to a window, so an invisible InputOnly child
        // placed over the wanted rectangle makes the rectangle the limit.
        static unordered_map<::Window, ::Window> confine_windows;
        if (const auto found = confine_windows.find(window); found != confine_windows.end()) {
            XUngrabPointer(display, CurrentTime);
            XDestroyWindow(display, found->second);
            confine_windows.erase(found);
        }
        if (rect == nullptr) {
            XFlush(display);
            return true;
        }
        if (rect->width <= 0 || rect->height <= 0) {
            return false;
        }
        const ::Window area = XCreateWindow(display, window, rect->x, rect->y, static_cast<unsigned>(rect->width),
                                            static_cast<unsigned>(rect->height), 0, 0 /* CopyFromParent depth */, InputOnly,
                                            nullptr /* CopyFromParent visual */, 0, nullptr);
        XMapWindow(display, area);
        const int status = XGrabPointer(display, window, True /* owner_events: our window still gets its clicks */,
                                        ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, area,
                                        0 /* keep the cursor */, CurrentTime);
        XFlush(display);
        if (status != 0 /* GrabSuccess */) {
            XDestroyWindow(display, area);
            return false;
        }
        confine_windows[window] = area;
        return true;
    }

    bool set_native_mouse_capture(void *window_handle, bool enabled) noexcept {
        if (!is_x11_window(window_handle)) {
            return false; // Wayland compositors keep delivering to the pressed-in surface on their own
        }
        auto *display = static_cast<Display *>(glfwGetX11Display());
        const ::Window window = glfwGetX11Window(static_cast<GLFWwindow *>(window_handle));
        if (!enabled) {
            XUngrabPointer(display, CurrentTime);
            XFlush(display);
            return true;
        }
        const int status = XGrabPointer(display, window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                                        GrabModeAsync, 0 /* no confinement */, 0 /* keep the cursor */, CurrentTime);
        XFlush(display);
        return status == 0 /* GrabSuccess */;
    }

    bool native_typed_clipboard_available() noexcept { return false; }
    bool native_clipboard_has(std::string_view) noexcept { return false; }
    std::vector<std::byte> native_clipboard_get(std::string_view) noexcept { return {}; }
    bool native_clipboard_set(void *, std::string_view, std::span<const std::byte>) noexcept { return false; }
    std::vector<std::string> native_clipboard_types() noexcept { return {}; }

    SystemTheme native_system_theme() noexcept {
        // Without a session-bus connection the portal's color-scheme setting is out of reach, so read what GTK and
        // KDE write to disk. The desktop's own switch may not be reflected until it rewrites these files.
        const char *home = std::getenv("HOME");
        const char *config_home = std::getenv("XDG_CONFIG_HOME");
        const std::string config = config_home != nullptr && config_home[0] != '\0' ? std::string(config_home)
                                   : home != nullptr                                ? std::string(home) + "/.config"
                                                                                    : std::string();
        if (config.empty()) {
            return SystemTheme::Unknown;
        }
        if (const char *gtk_theme = std::getenv("GTK_THEME"); gtk_theme != nullptr && gtk_theme[0] != '\0') {
            return lowercase(gtk_theme).find("dark") != std::string::npos ? SystemTheme::Dark : SystemTheme::Light;
        }
        const std::string kde = read_text_file(config + "/kdeglobals");
        if (const std::string scheme = ini_value(kde, "ColorScheme"); !scheme.empty()) {
            return lowercase(scheme).find("dark") != std::string::npos ? SystemTheme::Dark : SystemTheme::Light;
        }
        for (const char *version : {"gtk-4.0", "gtk-3.0"}) {
            const std::string gtk = read_text_file(config + "/" + version + "/settings.ini");
            if (const std::string prefer = ini_value(gtk, "gtk-application-prefer-dark-theme"); !prefer.empty()) {
                return prefer[0] == '1' || lowercase(prefer).rfind("true", 0) == 0 ? SystemTheme::Dark : SystemTheme::Light;
            }
            if (const std::string name = ini_value(gtk, "gtk-theme-name"); !name.empty()) {
                return lowercase(name).find("dark") != std::string::npos ? SystemTheme::Dark : SystemTheme::Light;
            }
        }
        return SystemTheme::Unknown;
    }

#else

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

    bool set_native_cursor_confine(void *window_handle, const WindowRect *rect) noexcept {
        (void)window_handle;
        (void)rect;
        return false;
    }

    bool set_native_mouse_capture(void *, bool) noexcept { return false; }

    bool native_typed_clipboard_available() noexcept { return false; }
    bool native_clipboard_has(std::string_view) noexcept { return false; }
    std::vector<std::byte> native_clipboard_get(std::string_view) noexcept { return {}; }
    bool native_clipboard_set(void *, std::string_view, std::span<const std::byte>) noexcept { return false; }
    std::vector<std::string> native_clipboard_types() noexcept { return {}; }

    SystemTheme native_system_theme() noexcept { return SystemTheme::Unknown; }

#endif // defined(__linux__)

} // namespace SFT::WindowManager::GLFW::Detail
