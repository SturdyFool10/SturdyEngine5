#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#if defined(__APPLE__)
#include <SDL3/SDL.h>

#include <CoreGraphics/CGGeometry.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#endif
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowConfig.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowEffects.hpp>
#include <WindowManager/WindowLog.hpp>
#include <WindowManager/WindowNative.hpp>

using std::expected;
using std::unexpected;

#if defined(__APPLE__)
// AppKit is driven through the Objective-C runtime (objc_msgSend) so this stays a plain C++ translation unit and the
// build needs no Objective-C++ language support. Written from Apple's documentation without an Apple toolchain to
// compile or run it on (see plans/vertex-launcher-gaps.md): treat it as untested until it has run on a Mac.
namespace SFT::WindowManager::Detail {

    namespace {

        template <typename Result, typename... Args>
        Result send(id target, const char *selector, Args... args) noexcept {
            return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(target, sel_registerName(selector), args...);
        }

        /// `-[NSView bounds]` returns a 32-byte struct: indirect return through objc_msgSend on arm64, but
        /// objc_msgSend_stret on x86_64.
        CGRect view_bounds(id view) noexcept {
#if defined(__arm64__) || defined(__aarch64__)
            return send<CGRect>(view, "bounds");
#else
            CGRect rect{};
            reinterpret_cast<void (*)(CGRect *, id, SEL)>(objc_msgSend_stret)(&rect, view, sel_registerName("bounds"));
            return rect;
#endif
        }

        // Associated-object key under which the window keeps its effect view alive.
        const char effect_view_key = 0;

        // NSVisualEffectMaterial raw values, indexed by `MacVisualEffectMaterial`.
        constexpr long material_values[] = {3, 4, 5, 6, 7, 10, 11, 12, 13, 15, 17, 18, 21, 22};
        static_assert(sizeof(material_values) / sizeof(material_values[0]) ==
                      static_cast<usize>(MacVisualEffectMaterial::UnderPageBackground) + 1);

        id appearance_for(MacAppearance appearance) noexcept {
            const char *name = nullptr;
            switch (appearance) {
                case MacAppearance::Inherit: return nullptr;
                case MacAppearance::Aqua: name = "NSAppearanceNameAqua"; break;
                case MacAppearance::DarkAqua: name = "NSAppearanceNameDarkAqua"; break;
                case MacAppearance::VibrantLight: name = "NSAppearanceNameVibrantLight"; break;
                case MacAppearance::VibrantDark: name = "NSAppearanceNameVibrantDark"; break;
            }
            const id string = send<id>(reinterpret_cast<id>(objc_getClass("NSString")), "stringWithUTF8String:", name);
            return send<id>(reinterpret_cast<id>(objc_getClass("NSAppearance")), "appearanceNamed:", string);
        }

        struct CocoaEffectJob {
            id window = nullptr;
            WindowEffect effect{};
            WindowEffectResult result = WindowEffectResult::failed("not run");
        };

        void make_window_see_through(id window) noexcept {
            send<void>(window, "setOpaque:", static_cast<signed char>(0));
            const id clear = send<id>(reinterpret_cast<id>(objc_getClass("NSColor")), "clearColor");
            send<void>(window, "setBackgroundColor:", clear);
        }

        /// Runs on the main thread (AppKit requires it).
        WindowEffectResult run_cocoa_effect(id window, const WindowEffect &effect) noexcept {
            const id existing = reinterpret_cast<id>(objc_getAssociatedObject(window, &effect_view_key));

            if (!effect.enabled) {
                if (existing != nullptr) {
                    send<void>(existing, "removeFromSuperview");
                    objc_setAssociatedObject(window, &effect_view_key, nullptr, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                }
                return WindowEffectResult::success("macOS visual effect removed.");
            }

            make_window_see_through(window);
            if (effect.kind == WindowEffectKind::Transparent) {
                return WindowEffectResult::success("macOS window made transparent.");
            }

            id view = existing;
            if (view == nullptr) {
                const Class view_class = objc_getClass("NSVisualEffectView");
                if (view_class == nullptr) {
                    return WindowEffectResult::failed("NSVisualEffectView is not available.");
                }
                const id content = send<id>(window, "contentView");
                if (content == nullptr) {
                    return WindowEffectResult::failed("The NSWindow has no content view.");
                }
                view = send<id>(send<id>(reinterpret_cast<id>(view_class), "alloc"), "init");
                if (view == nullptr) {
                    return WindowEffectResult::failed("Could not create an NSVisualEffectView.");
                }
                send<void>(view, "setFrame:", view_bounds(content));
                // NSViewWidthSizable (2) | NSViewHeightSizable (16): follow the window as it resizes.
                send<void>(view, "setAutoresizingMask:", static_cast<unsigned long>(2 | 16));
                // NSWindowBelow (-1): behind the existing content (the swapchain view).
                send<void>(content, "addSubview:positioned:relativeTo:", view, static_cast<long>(-1), static_cast<id>(nullptr));
                objc_setAssociatedObject(window, &effect_view_key, view, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                send<void>(view, "release"); // the associated object now owns it
            }

            const WindowEffectOptions &options = effect.options;
            send<void>(view, "setMaterial:", material_values[static_cast<usize>(options.mac_material)]);
            send<void>(view, "setBlendingMode:", static_cast<long>(options.mac_blending));
            send<void>(view, "setState:", static_cast<long>(options.mac_state));
            send<void>(view, "setEmphasized:", static_cast<signed char>(options.mac_emphasized ? 1 : 0));
            send<void>(view, "setAppearance:", appearance_for(options.mac_appearance));
            return WindowEffectResult::success("macOS NSVisualEffectView applied.");
        }

        void run_cocoa_job(void *context) noexcept {
            auto *job = static_cast<CocoaEffectJob *>(context);
            job->result = run_cocoa_effect(job->window, job->effect);
        }

        void run_cocoa_job_and_free(void *context) noexcept {
            run_cocoa_job(context);
            delete static_cast<CocoaEffectJob *>(context);
        }

    } // namespace

    WindowEffectResult apply_cocoa_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
        const id window = static_cast<id>(handle.window);
        if (pthread_main_np() != 0) {
            CocoaEffectJob job{window, effect};
            run_cocoa_job(&job);
            return job.result;
        }
        // Off the main thread: hop over asynchronously. Waiting would deadlock whenever the main thread is blocked
        // on a lock this thread holds, so the verdict is "queued", not the final result.
        dispatch_async_f(dispatch_get_main_queue(), new CocoaEffectJob{window, effect}, run_cocoa_job_and_free);
        return WindowEffectResult::success("macOS effect queued for the main thread.");
    }

} // namespace SFT::WindowManager::Detail
#endif

namespace SFT::WindowManager {

    /// Returns the current or globally available current operating system value.
    ///
    /// @return Returns the current current operating system value.
    /// @note This function does not throw exceptions.
    OperatingSystem current_operating_system() noexcept {
#if defined(__APPLE__)
        return OperatingSystem::MacOS;
#else
        return OperatingSystem::Unknown;
#endif
    }

    /// Performs the operating system may support window effect operation for `Windowing` using the supplied arguments.
    ///
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool operating_system_may_support_window_effect(WindowEffectKind effect) noexcept {
#if defined(__APPLE__)
        // `Blur` is the generic request and maps to the default NSVisualEffectView; `Transparent` clears the window
        // background so the (non-opaque) swapchain shows through.
        return effect == WindowEffectKind::VisualEffect || effect == WindowEffectKind::Blur ||
               effect == WindowEffectKind::Transparent;
#else
        (void)effect;
        return false;
#endif
    }

    /// Releases native window effects using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param release_display `release_display` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void release_native_window_effects(NativeWindowHandle handle, bool release_display) noexcept {
        (void)handle;
        (void)release_display;
    }

    /// Enables native window effect using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param effect `effect` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    WindowEffectSupport query_native_window_effect_support(NativeWindowHandle handle, WindowEffectKind kind) noexcept {
        (void)handle;
        WindowEffectSupport support;
#if defined(__APPLE__)
        if (!operating_system_may_support_window_effect(kind)) {
            support.add("none", EffectAvailability::Unavailable, "macOS offers NSVisualEffectView and window transparency only");
            return support;
        }
        if (kind == WindowEffectKind::Transparent) {
            support.add("NSWindow opaque = NO", EffectAvailability::Available);
        } else if (objc_getClass("NSVisualEffectView") != nullptr) {
            support.add("NSVisualEffectView", EffectAvailability::Available);
        } else {
            support.add("NSVisualEffectView", EffectAvailability::Unavailable, "the AppKit class is missing");
        }
#else
        (void)kind;
        support.add("none", EffectAvailability::Unavailable, "not an Apple build");
#endif
        return support;
    }

    WindowEffectResult enable_native_window_effect(NativeWindowHandle handle, WindowEffect effect) noexcept {
#if defined(__APPLE__)
        if (!operating_system_may_support_window_effect(effect.kind)) [[unlikely]] {
            return WindowEffectResult::failed("This window effect is not supported on macOS.");
        }
        if (handle.system != NativeWindowSystem::Cocoa || handle.window == nullptr) {
            return WindowEffectResult::failed("macOS window effects require an NSWindow.");
        }
        return Detail::apply_cocoa_effect(handle, effect);
#else
        (void)handle;
        Detail::window_warn(
            "macOS window effect implementation called on non-Apple build: kind={} enabled={} color_argb=0x{:08X}",
            static_cast<int>(effect.kind),
            effect.enabled,
            effect.color_argb);
        return WindowEffectResult::failed("macOS window effects are only available on Apple builds.");
#endif
    }

} // namespace SFT::WindowManager

namespace SFT::WindowManager::Detail {


    /// Performs the native window handle from SDL operation for `Detail` using the supplied arguments.
    ///
    /// @param window_handle Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OperationFailed`, `WindowErrorCode::Unsupported`.
    /// @note This function does not throw exceptions.
    expected<NativeWindowHandle, WindowError> native_window_handle_from_sdl(void *window_handle) noexcept {
#if defined(__APPLE__)
        auto *window = static_cast<SDL_Window *>(window_handle);
        if (!window) [[unlikely]] {
            Detail::window_error("SDL3 Cocoa native handle rejected null window.");
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Cocoa native handle requires a live window."});
        }

        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        NativeWindowHandle handle{
            NativeWindowSystem::Cocoa,
            nullptr,
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr),
        };
        if (!handle.window) [[unlikely]] {
            Detail::window_error("SDL3 Cocoa native handle missing NSWindow: sdl_window={} properties={}", static_cast<void *>(window), properties);
            return unexpected(WindowError{WindowErrorCode::OperationFailed, "SDL3 Cocoa native handle is incomplete."});
        }
        Detail::window_debug("SDL3 Cocoa native handle resolved: sdl_window={} properties={} ns_window={}", static_cast<void *>(window), properties, handle.window);
        return handle;
#else
        (void)window_handle;
        return unexpected(WindowError{WindowErrorCode::Unsupported, "SDL3 Cocoa native handles are only available on Apple builds."});
#endif
    }

} // namespace SFT::WindowManager::Detail
