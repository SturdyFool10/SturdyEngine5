#include <Foundation/Foundation.hpp>

#if defined(__linux__) || defined(__FreeBSD__)

#pragma region Imports
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#pragma endregion

#include <WindowManager/Linux/X11BlurProperty.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::WindowManager::Detail {

    namespace {

        // The few Xlib declarations needed, spelled out so no X11 development header is required. Atom and Window are
        // XIDs (unsigned long); property data of format 32 is an array of `long` on every platform Xlib supports.
        using XDisplay = void;
        using XId = unsigned long;
        constexpr XId xa_cardinal = 6;
        constexpr int prop_mode_replace = 0;

        struct XlibApi {
            XId (*intern_atom)(XDisplay *, const char *, int) = nullptr;
            int (*change_property)(XDisplay *, XId, XId, XId, int, int, const unsigned char *, int) = nullptr;
            int (*delete_property)(XDisplay *, XId, XId) = nullptr;
            int (*flush)(XDisplay *) = nullptr;
            // Optional: only the window-behaviour features below need these, so their absence does not make Xlib unusable.
            int (*set_transient_for_hint)(XDisplay *, XId, XId) = nullptr;
            void *(*get_wm_hints)(XDisplay *, XId) = nullptr;
            int (*set_wm_hints)(XDisplay *, XId, void *) = nullptr;
            int (*grab_keyboard)(XDisplay *, XId, int, int, int, unsigned long) = nullptr;
            int (*ungrab_keyboard)(XDisplay *, unsigned long) = nullptr;
            int (*send_event)(XDisplay *, XId, int, long, void *) = nullptr;
            XId (*default_root_window)(XDisplay *) = nullptr;
            int (*free_memory)(void *) = nullptr;
            bool usable = false;
        };

        const XlibApi &xlib() noexcept {
            static const XlibApi api = [] {
                XlibApi loaded;
                // SDL has normally loaded libX11 already; RTLD_NOLOAD finds that copy without loading a second one.
                void *library = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
                if (library == nullptr) {
                    library = dlopen("libX11.so.6", RTLD_LAZY);
                }
                if (library == nullptr) {
                    return loaded;
                }
                loaded.intern_atom = reinterpret_cast<decltype(loaded.intern_atom)>(dlsym(library, "XInternAtom"));
                loaded.change_property = reinterpret_cast<decltype(loaded.change_property)>(dlsym(library, "XChangeProperty"));
                loaded.flush = reinterpret_cast<decltype(loaded.flush)>(dlsym(library, "XFlush"));
                loaded.delete_property = reinterpret_cast<decltype(loaded.delete_property)>(dlsym(library, "XDeleteProperty"));
                loaded.set_transient_for_hint = reinterpret_cast<decltype(loaded.set_transient_for_hint)>(dlsym(library, "XSetTransientForHint"));
                loaded.get_wm_hints = reinterpret_cast<decltype(loaded.get_wm_hints)>(dlsym(library, "XGetWMHints"));
                loaded.set_wm_hints = reinterpret_cast<decltype(loaded.set_wm_hints)>(dlsym(library, "XSetWMHints"));
                loaded.grab_keyboard = reinterpret_cast<decltype(loaded.grab_keyboard)>(dlsym(library, "XGrabKeyboard"));
                loaded.ungrab_keyboard = reinterpret_cast<decltype(loaded.ungrab_keyboard)>(dlsym(library, "XUngrabKeyboard"));
                loaded.send_event = reinterpret_cast<decltype(loaded.send_event)>(dlsym(library, "XSendEvent"));
                loaded.default_root_window = reinterpret_cast<decltype(loaded.default_root_window)>(dlsym(library, "XDefaultRootWindow"));
                loaded.free_memory = reinterpret_cast<decltype(loaded.free_memory)>(dlsym(library, "XFree"));
                loaded.usable = loaded.intern_atom != nullptr && loaded.change_property != nullptr &&
                                loaded.delete_property != nullptr && loaded.flush != nullptr;
                return loaded;
            }();
            return api;
        }

        // The X Shape extension (libXext): a window's bounding shape is the outline the server (and a compositor)
        // clips it to. `XRectangle` is {short x, y; unsigned short width, height}.
        struct XRectangle {
            short x;
            short y;
            unsigned short width;
            unsigned short height;
        };
        constexpr int shape_bounding = 0;
        constexpr int shape_set = 0;
        constexpr int rect_ordering_unsorted = 0;

        struct XShapeApi {
            void (*combine_rectangles)(XDisplay *, XId, int, int, int, XRectangle *, int, int, int) = nullptr;
            void (*combine_mask)(XDisplay *, XId, int, int, int, XId, int) = nullptr;
            bool usable = false;
        };

        const XShapeApi &xshape() noexcept {
            static const XShapeApi api = [] {
                XShapeApi loaded;
                void *library = dlopen("libXext.so.6", RTLD_LAZY | RTLD_NOLOAD);
                if (library == nullptr) {
                    library = dlopen("libXext.so.6", RTLD_LAZY);
                }
                if (library == nullptr) {
                    return loaded;
                }
                loaded.combine_rectangles =
                    reinterpret_cast<decltype(loaded.combine_rectangles)>(dlsym(library, "XShapeCombineRectangles"));
                loaded.combine_mask = reinterpret_cast<decltype(loaded.combine_mask)>(dlsym(library, "XShapeCombineMask"));
                loaded.usable = loaded.combine_rectangles != nullptr && loaded.combine_mask != nullptr;
                return loaded;
            }();
            return api;
        }

    } // namespace

    WindowEffectResult set_x11_corner_radius(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_x11_corner_radius");
        if (handle.system != NativeWindowSystem::X11 || handle.display == nullptr || handle.window == nullptr) {
            return WindowEffectResult::failed("X11 window corners require an X11 display and window.");
        }
        const XlibApi &api = xlib();
        const XShapeApi &shape = xshape();
        if (!api.usable || !shape.usable) {
            return WindowEffectResult::failed("libX11/libXext could not be loaded for the X Shape extension.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        const XId window = static_cast<XId>(reinterpret_cast<std::uintptr_t>(handle.window));
        const WindowEffectOptions &options = effect.options;
        const f32 scale = options.pixel_scale > 0.0f ? options.pixel_scale : 1.0f;
        const u32 width = static_cast<u32>(std::lround(static_cast<f32>(options.window_width) * scale));
        const u32 height = static_cast<u32>(std::lround(static_cast<f32>(options.window_height) * scale));

        if (!effect.enabled || options.window_corner_radius <= 0.0f || width == 0 || height == 0) {
            shape.combine_mask(display, window, shape_bounding, 0, 0, 0 /* None: back to the rectangular default */, shape_set);
            api.flush(display);
            return WindowEffectResult::success("X11 window shape reset to a rectangle.");
        }

        // The blur-region builder already turns "this rectangle with rounded corners" into scanline rectangles.
        WindowEffectOptions outline{};
        outline.corner_radius = options.window_corner_radius * scale;
        outline.window_width = width;
        outline.window_height = height;
        const std::vector<EffectRect> rects = resolve_blur_region(outline);
        if (rects.empty()) {
            return WindowEffectResult::failed("Could not build the rounded window outline.");
        }
        std::vector<XRectangle> shape_rects;
        shape_rects.reserve(rects.size());
        for (const EffectRect &rect : rects) {
            shape_rects.push_back(XRectangle{static_cast<short>(rect.x), static_cast<short>(rect.y),
                                             static_cast<unsigned short>(rect.width), static_cast<unsigned short>(rect.height)});
        }
        shape.combine_rectangles(display, window, shape_bounding, 0, 0, shape_rects.data(), static_cast<int>(shape_rects.size()),
                                 shape_set, rect_ordering_unsorted);
        api.flush(display);
        window_info("X11 window shape set ({} rectangle(s)).", shape_rects.size());
        return WindowEffectResult::success("X11 window shape applied (hard-edged corners).");
    }

    WindowEffectResult set_x11_kde_blur(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_x11_kde_blur");
        if (handle.system != NativeWindowSystem::X11 || handle.display == nullptr || handle.window == nullptr) {
            return WindowEffectResult::failed("X11 blur requires an X11 display and window.");
        }
        const XlibApi &api = xlib();
        if (!api.usable) {
            return WindowEffectResult::failed("libX11 could not be loaded for the X11 blur property.");
        }

        auto *display = static_cast<XDisplay *>(handle.display);
        const XId window = static_cast<XId>(reinterpret_cast<std::uintptr_t>(handle.window));
        const XId atom = api.intern_atom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", 0);
        if (atom == 0) {
            return WindowEffectResult::failed("Could not intern _KDE_NET_WM_BLUR_BEHIND_REGION.");
        }

        if (!effect.enabled) {
            api.delete_property(display, window, atom);
            api.flush(display);
            return WindowEffectResult::success("X11 blur property removed.");
        }

        // KWin reads the property in device pixels. A property with no rectangles means the whole window.
        const f32 scale = effect.options.pixel_scale > 0.0f ? effect.options.pixel_scale : 1.0f;
        std::vector<long> data;
        for (const EffectRect &rect : resolve_blur_region(effect.options)) {
            data.push_back(static_cast<long>(std::lround(static_cast<f32>(rect.x) * scale)));
            data.push_back(static_cast<long>(std::lround(static_cast<f32>(rect.y) * scale)));
            data.push_back(static_cast<long>(std::lround(static_cast<f32>(rect.width) * scale)));
            data.push_back(static_cast<long>(std::lround(static_cast<f32>(rect.height) * scale)));
        }
        api.change_property(display, window, atom, xa_cardinal, 32, prop_mode_replace,
                            reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()));
        api.flush(display);
        window_info("X11 background blur property set ({} value(s)).", data.size());
        return WindowEffectResult::success("X11 _KDE_NET_WM_BLUR_BEHIND_REGION set; it takes effect only if the window manager reads it.");
    }

    WindowEffectResult set_x11_window_opacity(NativeWindowHandle handle, WindowEffect effect) noexcept {
        ZoneScopedN("Windowing::set_x11_window_opacity");
        if (handle.system != NativeWindowSystem::X11 || handle.display == nullptr || handle.window == nullptr) {
            return WindowEffectResult::failed("X11 opacity requires an X11 display and window.");
        }
        const XlibApi &api = xlib();
        if (!api.usable) {
            return WindowEffectResult::failed("libX11 could not be loaded for the X11 opacity property.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        const XId window = static_cast<XId>(reinterpret_cast<std::uintptr_t>(handle.window));
        const XId atom = api.intern_atom(display, "_NET_WM_WINDOW_OPACITY", 0);
        if (atom == 0) {
            return WindowEffectResult::failed("Could not intern _NET_WM_WINDOW_OPACITY.");
        }
        // Full opacity is "no property" so a compositor that ignores it costs nothing.
        const f32 opacity = std::clamp(effect.options.opacity, 0.0f, 1.0f);
        if (!effect.enabled || opacity >= 1.0f) {
            api.delete_property(display, window, atom);
            api.flush(display);
            return WindowEffectResult::success("X11 window opacity property removed.");
        }
        const long value = static_cast<long>(static_cast<double>(opacity) * 4294967295.0);
        api.change_property(display, window, atom, xa_cardinal, 32, prop_mode_replace,
                            reinterpret_cast<const unsigned char *>(&value), 1);
        api.flush(display);
        return WindowEffectResult::success("X11 _NET_WM_WINDOW_OPACITY set; it takes effect only if the compositor reads it.");
    }

    WindowEffectResult set_x11_dark_mode(NativeWindowHandle handle, bool enabled) noexcept {
        ZoneScopedN("Windowing::set_x11_dark_mode");
        if (handle.system != NativeWindowSystem::X11 || handle.display == nullptr || handle.window == nullptr) {
            return WindowEffectResult::failed("X11 dark mode requires an X11 display and window.");
        }
        const XlibApi &api = xlib();
        if (!api.usable) {
            return WindowEffectResult::failed("libX11 could not be loaded for the X11 theme variant property.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        const XId window = static_cast<XId>(reinterpret_cast<std::uintptr_t>(handle.window));
        const XId atom = api.intern_atom(display, "_GTK_THEME_VARIANT", 0);
        const XId utf8 = api.intern_atom(display, "UTF8_STRING", 0);
        if (atom == 0 || utf8 == 0) {
            return WindowEffectResult::failed("Could not intern _GTK_THEME_VARIANT.");
        }
        if (!enabled) {
            api.delete_property(display, window, atom);
        } else {
            static constexpr unsigned char dark[] = "dark";
            api.change_property(display, window, atom, utf8, 8, prop_mode_replace, dark, 4);
        }
        api.flush(display);
        return WindowEffectResult::success("X11 _GTK_THEME_VARIANT set; it applies to window-manager-drawn (GTK) decorations.");
    }

    namespace {

        [[nodiscard]] bool x11_ready(const NativeWindowHandle &handle) noexcept {
            return handle.system == NativeWindowSystem::X11 && handle.display != nullptr && handle.window != nullptr && xlib().usable;
        }

        [[nodiscard]] XId x11_window(const NativeWindowHandle &handle) noexcept {
            return static_cast<XId>(reinterpret_cast<std::uintptr_t>(handle.window));
        }

        // XClientMessageEvent, spelled out like the rest of Xlib here; XSendEvent takes a pointer to the XEvent union
        // whose first member this is.
        struct XClientMessageRaw {
            int type;
            unsigned long serial;
            int send_event;
            XDisplay *display;
            XId window;
            XId message_type;
            int format;
            long data[5];
        };
        constexpr int x_client_message = 33;
        constexpr long x_substructure_notify_mask = 1L << 19;
        constexpr long x_substructure_redirect_mask = 1L << 20;
        constexpr int shape_input = 2;

        // XWMHints (Xutil.h) and its InputHint flag.
        struct XWmHintsRaw {
            long flags;
            int input;
            int initial_state;
            XId icon_pixmap;
            XId icon_window;
            int icon_x;
            int icon_y;
            XId icon_mask;
            XId window_group;
        };
        constexpr long x_input_hint = 1L << 0;

        /// A `_NET_WM_STATE` add/remove request for one state atom, sent to the root window as EWMH describes.
        void x11_send_net_wm_state(const NativeWindowHandle &handle, const char *state_name, bool add) noexcept {
            const XlibApi &api = xlib();
            if (api.send_event == nullptr || api.default_root_window == nullptr) {
                return;
            }
            auto *display = static_cast<XDisplay *>(handle.display);
            union {
                XClientMessageRaw message;
                alignas(8) unsigned char padding[192]; // sizeof(XEvent)
            } event{};
            event.message.type = x_client_message;
            event.message.display = display;
            event.message.window = x11_window(handle);
            event.message.message_type = api.intern_atom(display, "_NET_WM_STATE", 0);
            event.message.format = 32;
            event.message.data[0] = add ? 1 : 0; // _NET_WM_STATE_ADD / _NET_WM_STATE_REMOVE
            event.message.data[1] = static_cast<long>(api.intern_atom(display, state_name, 0));
            event.message.data[3] = 1; // source indication: normal application
            api.send_event(display, api.default_root_window(display), 0, x_substructure_notify_mask | x_substructure_redirect_mask, &event);
        }

    } // namespace

    WindowEffectResult set_x11_mouse_passthrough(NativeWindowHandle handle, bool enabled) noexcept {
        ZoneScopedN("Windowing::set_x11_mouse_passthrough");
        const XShapeApi &shape = xshape();
        if (!x11_ready(handle) || !shape.usable) {
            return WindowEffectResult::failed("X11 click-through needs an X11 window and libXext (X Shape extension).");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        if (enabled) {
            // An empty input shape: the window still draws, but no pointer event is delivered to it.
            shape.combine_rectangles(display, x11_window(handle), shape_input, 0, 0, nullptr, 0, shape_set, rect_ordering_unsorted);
        } else {
            shape.combine_mask(display, x11_window(handle), shape_input, 0, 0, 0 /* None */, shape_set);
        }
        xlib().flush(display);
        return WindowEffectResult::success("X11 input shape set.");
    }

    WindowEffectResult set_x11_focusable(NativeWindowHandle handle, bool focusable) noexcept {
        ZoneScopedN("Windowing::set_x11_focusable");
        const XlibApi &api = xlib();
        if (!x11_ready(handle) || api.get_wm_hints == nullptr || api.set_wm_hints == nullptr) {
            return WindowEffectResult::failed("X11 focus control needs an X11 window and WM_HINTS access.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        auto *existing = static_cast<XWmHintsRaw *>(api.get_wm_hints(display, x11_window(handle)));
        XWmHintsRaw hints{};
        if (existing != nullptr) {
            hints = *existing;
            if (api.free_memory != nullptr) {
                api.free_memory(existing);
            }
        }
        // ICCCM: input = False means the window manager must not give the window the input focus on a click.
        hints.flags |= x_input_hint;
        hints.input = focusable ? 1 : 0;
        api.set_wm_hints(display, x11_window(handle), &hints);
        api.flush(display);
        return WindowEffectResult::success("X11 WM_HINTS input flag set; it takes effect if the window manager honours it.");
    }

    WindowEffectResult set_x11_keyboard_grab(NativeWindowHandle handle, bool enabled) noexcept {
        ZoneScopedN("Windowing::set_x11_keyboard_grab");
        const XlibApi &api = xlib();
        if (!x11_ready(handle) || api.grab_keyboard == nullptr || api.ungrab_keyboard == nullptr) {
            return WindowEffectResult::failed("X11 keyboard grab needs an X11 window.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        if (!enabled) {
            api.ungrab_keyboard(display, 0 /* CurrentTime */);
            api.flush(display);
            return WindowEffectResult::success("X11 keyboard released.");
        }
        constexpr int grab_mode_async = 1;
        const int status = api.grab_keyboard(display, x11_window(handle), 1 /* owner_events */, grab_mode_async, grab_mode_async, 0 /* CurrentTime */);
        api.flush(display);
        if (status != 0 /* GrabSuccess */) {
            return WindowEffectResult::failed("The X server refused the keyboard grab (the window must be visible, and no other client may hold the keyboard).");
        }
        return WindowEffectResult::success("X11 keyboard grabbed.");
    }

    WindowEffectResult set_x11_window_parent(NativeWindowHandle child, std::optional<NativeWindowHandle> parent, bool modal) noexcept {
        ZoneScopedN("Windowing::set_x11_window_parent");
        const XlibApi &api = xlib();
        if (!x11_ready(child) || api.set_transient_for_hint == nullptr) {
            return WindowEffectResult::failed("X11 window parenting needs an X11 window.");
        }
        auto *display = static_cast<XDisplay *>(child.display);
        if (parent) {
            if (!x11_ready(*parent)) {
                return WindowEffectResult::failed("The parent window is not an X11 window.");
            }
            api.set_transient_for_hint(display, x11_window(child), x11_window(*parent)); // WM_TRANSIENT_FOR
        } else {
            api.delete_property(display, x11_window(child), api.intern_atom(display, "WM_TRANSIENT_FOR", 0));
        }
        x11_send_net_wm_state(child, "_NET_WM_STATE_MODAL", parent.has_value() && modal);
        api.flush(display);
        return WindowEffectResult::success("X11 WM_TRANSIENT_FOR set.");
    }

    WindowEffectResult set_x11_window_type(NativeWindowHandle handle, WindowType type) noexcept {
        ZoneScopedN("Windowing::set_x11_window_type");
        const XlibApi &api = xlib();
        if (!x11_ready(handle)) {
            return WindowEffectResult::failed("X11 window types need an X11 window.");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        const char *name = "_NET_WM_WINDOW_TYPE_NORMAL";
        switch (type) {
            case WindowType::Normal: name = "_NET_WM_WINDOW_TYPE_NORMAL"; break;
            case WindowType::Utility: name = "_NET_WM_WINDOW_TYPE_UTILITY"; break;
            case WindowType::Tooltip: name = "_NET_WM_WINDOW_TYPE_TOOLTIP"; break;
            case WindowType::PopupMenu: name = "_NET_WM_WINDOW_TYPE_POPUP_MENU"; break;
            case WindowType::DropdownMenu: name = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU"; break;
            case WindowType::Dialog: name = "_NET_WM_WINDOW_TYPE_DIALOG"; break;
            case WindowType::Splash: name = "_NET_WM_WINDOW_TYPE_SPLASH"; break;
        }
        // EWMH: the list is in order of preference, so a window manager that does not know the first falls back to
        // "normal".
        const long types[2] = {static_cast<long>(api.intern_atom(display, name, 0)),
                               static_cast<long>(api.intern_atom(display, "_NET_WM_WINDOW_TYPE_NORMAL", 0))};
        constexpr XId xa_atom = 4;
        api.change_property(display, x11_window(handle), api.intern_atom(display, "_NET_WM_WINDOW_TYPE", 0), xa_atom, 32,
                            prop_mode_replace, reinterpret_cast<const unsigned char *>(types), type == WindowType::Normal ? 1 : 2);
        api.flush(display);
        return WindowEffectResult::success("X11 _NET_WM_WINDOW_TYPE set; most window managers read it only when the window is mapped.");
    }

    WindowEffectResult set_x11_window_shape(NativeWindowHandle handle, const WindowShapeMask &mask, WindowExtent pixel_size) noexcept {
        ZoneScopedN("Windowing::set_x11_window_shape");
        const XShapeApi &shape = xshape();
        if (!x11_ready(handle) || !shape.usable) {
            return WindowEffectResult::failed("X11 window shapes need an X11 window and libXext (X Shape extension).");
        }
        auto *display = static_cast<XDisplay *>(handle.display);
        if (mask.coverage.empty()) {
            shape.combine_mask(display, x11_window(handle), shape_bounding, 0, 0, 0 /* None */, shape_set);
            xlib().flush(display);
            return WindowEffectResult::success("X11 window shape reset to a rectangle.");
        }
        const std::vector<EffectRect> rects = resolve_shape_region(mask, pixel_size);
        if (rects.empty()) {
            return WindowEffectResult::failed("The shape mask covers nothing; a window needs at least one visible pixel.");
        }
        std::vector<XRectangle> shape_rects;
        shape_rects.reserve(rects.size());
        for (const EffectRect &rect : rects) {
            shape_rects.push_back(XRectangle{static_cast<short>(rect.x), static_cast<short>(rect.y), static_cast<unsigned short>(rect.width),
                                             static_cast<unsigned short>(rect.height)});
        }
        shape.combine_rectangles(display, x11_window(handle), shape_bounding, 0, 0, shape_rects.data(), static_cast<int>(shape_rects.size()),
                                 shape_set, rect_ordering_unsorted);
        xlib().flush(display);
        return WindowEffectResult::success("X11 window shape applied (hard-edged).");
    }

    WindowEffectSupport query_x11_blur_support(NativeWindowHandle handle) noexcept {
        WindowEffectSupport support;
        if (handle.system != NativeWindowSystem::X11) {
            support.add("_KDE_NET_WM_BLUR_BEHIND_REGION", EffectAvailability::Unavailable, "not an X11 window");
        } else if (!xlib().usable) {
            support.add("_KDE_NET_WM_BLUR_BEHIND_REGION", EffectAvailability::Unavailable, "libX11 is not available");
        } else {
            support.add("_KDE_NET_WM_BLUR_BEHIND_REGION", EffectAvailability::Unknown,
                        "X11 cannot report whether the window manager reads this property (KWin does)");
        }
        return support;
    }

} // namespace SFT::WindowManager::Detail

#endif
