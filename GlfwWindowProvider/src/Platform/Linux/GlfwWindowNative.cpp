#include <Foundation/src/Foundation.hpp>

#include <algorithm>
#include <cstdint>
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

#include <Platform/Window/WindowLog.hpp>
#include <Platform/Window/GLFW/GlfwWindowNative.hpp>

#include <tracy/Tracy.hpp>

using std::expected;
using std::unexpected;
using std::uintptr_t;
using std::unordered_map;
using std::vector;
using std::wstring;

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
        ZoneScopedN("Windowing::GLFW::Detail::native_window_handle");
#if defined(__linux__)
        auto *window = static_cast<GLFWwindow *>(window_handle);
        if (!window) [[unlikely]] {
            ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "GLFW Linux native handle requires a live window."});
        }

        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
            NativeWindowHandle handle{NativeWindowSystem::Wayland, glfwGetWaylandDisplay(), glfwGetWaylandWindow(window)};
            if (!handle.display || !handle.window) [[unlikely]] {
                ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux Wayland native handle missing display or surface: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
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
                ::SFT::Platform::Windowing::Detail::window_error("GLFW Linux X11 native handle missing display or window: glfw_window={} display={} window={}", static_cast<void *>(window), handle.display, handle.window);
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

#endif // defined(__linux__)

} // namespace SFT::Platform::Windowing::GLFW::Detail
