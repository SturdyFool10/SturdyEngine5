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

#if defined(__linux__)

    namespace {

        // ── X11: XIMPreeditCallbacks-style IME composition ──────────────────────────────────────
        //
        // Deliberately not intercepting GLFW's own X11 event loop (XFilterEvent/XNextEvent) at all
        // — XIMPreeditCallbacks input style makes the XIM server invoke these callbacks directly
        // whenever composition text changes, entirely out-of-band from ordinary X11 event delivery,
        // so GLFW's own key handling (which already correctly delivers final committed characters
        // for IME-composed text today) is completely undisturbed by any of this.

        // One XIC's full preedit state — Xlib's XIM API has no "get my callback's own client data
        // back" query outside the callback invocations themselves, so this file keeps its own map
        // from X11 Window XID to state instead (the `::` qualifier throughout this block picks X11's
        // global `Window` XID typedef over SFT::Platform::Windowing::Window, the class this whole
        // file's enclosing namespace would otherwise resolve a bare `Window` to).
        struct X11ImeState {
            XIC ic = nullptr;
            ImePreeditCallback callback = nullptr;
            void *userData = nullptr;
            // UTF-32 (wchar_t is 4 bytes on every mainstream Linux libc) accumulation buffer,
            // spliced in place per XIMPreeditDrawCallbackStruct's chg_first/chg_length/text fields
            // — converted to UTF-8 fresh each time the callback actually fires, not kept in sync
            // incrementally, since composition strings are short and this runs at typing speed, not
            // per-frame.
            wstring preeditBuffer;
        };

        [[nodiscard]] XIM &x11_shared_im() noexcept {
            // One XIM per process, opened lazily against whichever Display GLFW's own X11 backend is
            // already using, and never closed — X11 apps conventionally hold their XIM connection
            // for the process lifetime; the input method server outlives any single window anyway.
            static XIM im = nullptr;
            return im;
        }

        [[nodiscard]] unordered_map<::Window, X11ImeState *> &x11_ime_states() noexcept {
            static unordered_map<::Window, X11ImeState *> states;
            return states;
        }

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

        [[nodiscard]] std::string wstring_to_utf8(const wstring &text) noexcept {
            std::string result;
            result.reserve(text.size() * 2);
            for (wchar_t ch : text) {
                append_utf8(result, static_cast<char32_t>(static_cast<uint32_t>(ch)));
            }
            return result;
        }

        // XNPreeditStartCallback — return value is the maximum preedit length the application can
        // accept, or -1 for unbounded (this buffer is a resizable wstring, so always unbounded).
        int x11_preedit_start_callback(XIC /*ic*/, XPointer clientData, XPointer /*callData*/) {
            if (auto *state = reinterpret_cast<X11ImeState *>(clientData)) {
                state->preeditBuffer.clear();
            }
            return -1;
        }

        // XNPreeditDrawCallback — describes an in-place splice (delete chg_length chars starting at
        // chg_first, then insert call_data->text if non-null), not a flat replacement of the whole
        // string; call_data->caret is the new cursor offset (in characters) after the splice.
        void x11_preedit_draw_callback(XIC /*ic*/, XPointer clientData, XPointer callDataRaw) {
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
                        // Best-effort fallback: the XIM server reported locale multi-byte text
                        // instead of wide_char. mbrtowc() decodes it per the current locale, which
                        // is correct on any locale (not just UTF-8 ones) as long as the process
                        // locale was actually set (setlocale(LC_CTYPE, "")) — an XIM connection
                        // can't exist at all otherwise, so this is a safe assumption here.
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

        // XNPreeditDoneCallback — composition ended (confirmed or cancelled); empty text + cursorPos
        // -1 is this codebase's own "composition ended" contract (ImePreeditCallback's doc comment).
        void x11_preedit_done_callback(XIC /*ic*/, XPointer clientData, XPointer /*callData*/) {
            if (auto *state = reinterpret_cast<X11ImeState *>(clientData)) {
                state->preeditBuffer.clear();
                if (state->callback) {
                    state->callback("", -1, state->userData);
                }
            }
        }

        [[nodiscard]] bool x11_install_ime(::Window xwindow, ImePreeditCallback callback, void *userData) noexcept {
            Display *display = glfwGetX11Display();
            if (!display) [[unlikely]] {
                return false;
            }
            if (!x11_shared_im()) {
                // NULL modifiers/encoding args: accept the locale's own default input method,
                // exactly as XSetLocaleModifiers()'s own documented default behavior intends — this
                // engine doesn't second-guess the user's configured input method.
                x11_shared_im() = XOpenIM(display, nullptr, nullptr, nullptr);
                if (!x11_shared_im()) [[unlikely]] {
                    // No XIM server available (common on a minimal X setup with no input method
                    // running) — not an error, just "this session never gets composition events."
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

        void x11_set_exclude_rect(::Window xwindow, int x, int y) noexcept {
            auto it = x11_ime_states().find(xwindow);
            if (it == x11_ime_states().end() || !it->second->ic) {
                return;
            }
            // X11/XIM's positioning model is a spot point (where the candidate/status window
            // anchors), not an excluded rectangle like Win32's CFS_EXCLUDE — the rect's top-left is
            // the natural equivalent; the input method decides its own window placement from there,
            // same as every other X11 application.
            XPoint spot{static_cast<short>(x), static_cast<short>(y)};
            if (XVaNestedList list = XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr)) {
                XSetICValues(it->second->ic, XNPreeditAttributes, list, nullptr);
                XFree(list);
            }
        }

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

        // ── Wayland: zwp_text_input_v3 ───────────────────────────────────────────────────────────
        //
        // GLFW's own Wayland backend binds its own wl_seat/registry internally but exposes neither
        // via public API, so this binds an entirely independent wl_registry to the same wl_display
        // (glfwGetWaylandDisplay()) — standard, safe Wayland practice; GLFW's own registry binding
        // is untouched. Both stay on the *default* event queue (confirmed by inspecting GLFW's own
        // wl_init.c: no wl_display_create_queue/wl_proxy_set_queue anywhere), which is what makes
        // ongoing preedit_string/commit_string/done events arrive for free as a side effect of
        // GLFW's own wl_display_dispatch() inside its regular pollEvents() — no separate dispatch
        // loop needed on this side for steady-state operation.
        //
        // Caveat, flagged honestly rather than silently assumed: the *initial* global-binding
        // round-trip below (wl_display_roundtrip()) synchronously drains the default queue,
        // including whatever GLFW's own registry listener does in response to the same global
        // announcements — this has not been verified against a real compositor/GLFW combination in
        // this session (no Wayland environment available to test against). If this turns out to
        // cause reentrancy trouble with GLFW's own Wayland init path in practice, the fix is binding
        // our registry to a private queue for the startup round-trip only, then migrating the seat/
        // manager/text-input objects back onto the default queue before relying on GLFW's dispatch
        // for steady state — not attempted here since it can't be validated blind either.

        struct WaylandGlobals {
            wl_registry *registry = nullptr;
            wl_seat *seat = nullptr;
            zwp_text_input_manager_v3 *textInputManager = nullptr;
        };

        [[nodiscard]] WaylandGlobals &wayland_globals() noexcept {
            static WaylandGlobals globals;
            return globals;
        }

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

        void wayland_registry_global_remove(void * /*data*/, wl_registry * /*registry*/, uint32_t /*name*/) {}

        constexpr wl_registry_listener kRegistryListener{
            .global = wayland_registry_global,
            .global_remove = wayland_registry_global_remove,
        };

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
            // The protocol batches preedit_string/commit_string updates and only actually commits
            // them on a matching `done` event (serial-numbered) — buffered here and only forwarded
            // to `callback` once `done` fires, per the protocol's own documented contract.
            std::string pendingPreedit;
            bool pendingPreeditSet = false;
        };

        [[nodiscard]] unordered_map<wl_surface *, WaylandImeState *> &wayland_ime_states() noexcept {
            static unordered_map<wl_surface *, WaylandImeState *> states;
            return states;
        }

        void wayland_text_input_enter(void *, zwp_text_input_v3 *, wl_surface *) {
            // Focus tracking is implicit in the protocol via enter/leave, but this file keys its own
            // state by wl_surface* (set at install time) rather than reacting to enter/leave — a
            // GLFW window's own focus/lifetime already governs when install/remove are called, and
            // duplicating that as a second focus-tracking mechanism here would risk the two
            // disagreeing. Intentionally empty.
        }

        void wayland_text_input_leave(void *, zwp_text_input_v3 *, wl_surface *) {}

        void wayland_text_input_preedit_string(void *data, zwp_text_input_v3 *, const char *text,
                                               int32_t /*cursorBegin*/, int32_t /*cursorEnd*/) {
            auto *state = static_cast<WaylandImeState *>(data);
            if (!state) [[unlikely]] {
                return;
            }
            state->pendingPreedit = text ? text : "";
            state->pendingPreeditSet = true;
        }

        void wayland_text_input_commit_string(void * /*data*/, zwp_text_input_v3 * /*textInput*/, const char * /*text*/) {
            // Not forwarded through this hook — GLFW's own Wayland backend already delivers
            // committed text via its ordinary keyboard/text-input handling into the standard char
            // callback (glfw_char_callback, GLFWImpl.cpp), same as every other text a GLFW app
            // receives. Only the preedit half is new here.
        }

        void wayland_text_input_delete_surrounding_text(void *, zwp_text_input_v3 *, uint32_t, uint32_t) {}

        void wayland_text_input_done(void *data, zwp_text_input_v3 *, uint32_t /*serial*/) {
            auto *state = static_cast<WaylandImeState *>(data);
            if (!state || !state->pendingPreeditSet || !state->callback) [[unlikely]] {
                return;
            }
            // An empty preedit_string payload (or one never sent before this done) means
            // composition ended — matches this codebase's own "empty = ended" contract
            // (ImePreeditCallback's own doc comment) with no special-casing needed here.
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

        void wayland_set_exclude_rect(wl_surface *surface, int x, int y, int width, int height) noexcept {
            auto it = wayland_ime_states().find(surface);
            if (it == wayland_ime_states().end() || !it->second->textInput) {
                return;
            }
            // The protocol's own direct equivalent of Win32's composition-exclusion rect — no
            // translation needed, unlike X11's spot-point model.
            zwp_text_input_v3_set_cursor_rectangle(it->second->textInput, x, y, width, height);
            zwp_text_input_v3_commit(it->second->textInput);
        }

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
            // The protocol requires a matching commit() after enable()/disable() (and after any
            // state-changing request) before the compositor applies it — see set_cursor_rectangle's
            // own commit() call above for the same rule.
            zwp_text_input_v3_commit(it->second->textInput);
        }

    } // namespace

#endif // defined(STURDY_GLFW_WAYLAND_TEXT_INPUT_V3)

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

#else // !defined(__linux__)

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

#endif // defined(__linux__)

} // namespace SFT::Platform::Windowing::GLFW::Detail
