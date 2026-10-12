#include <Foundation/Foundation.hpp>

#if defined(__linux__) || defined(__FreeBSD__)

#pragma region Imports
#include <dlfcn.h>

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
                loaded.usable = loaded.intern_atom != nullptr && loaded.change_property != nullptr &&
                                loaded.delete_property != nullptr && loaded.flush != nullptr;
                return loaded;
            }();
            return api;
        }

    } // namespace

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
