#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>
#pragma endregion

#include <Platform/Window/WindowError.hpp>
#include <Platform/Window/WindowGeometry.hpp>
#include <Platform/Window/WindowEvent.hpp>
#include <Platform/Window/WindowEffect.hpp>
#include <Platform/Window/WindowConfig.hpp>

using std::bad_alloc;
using std::derived_from;
using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::Platform::Windowing {

    // Which windowing **library** is driving a window — the concrete abstraction backend behind the
    // `Window` interface.
    //
    // Switch on this only when a library-specific path is unavoidable, e.g. picking the matching
    // Vulkan surface-creation provider (see `Core::Vulkan::to_surface_provider`). Reported by
    // `Window::backend_kind()`.
    enum class WindowBackendKind {
        SDL3,
        GLFW,
    };

    // The windowing **system** a window belongs to, reported by `Window::type()`.
    //
    // Each backend currently reports its own library identity here, so today this coincides with
    // `WindowBackendKind`; it is kept a separate query so callers that only care about the windowing
    // system aren't coupled to the engine's backend-selection enum.
    //
    // - `Unknown` — not yet determined / undeterminable.
    // - `SDL3` / `GLFW` — the backing library.
    enum class WindowingSystem {
        Unknown,
        SDL3,
        GLFW,
    };

    // Stable, process-unique window identity — assigned once at construction and **never reused**.
    //
    // Engine backends key every per-window GPU resource (surface, swapchain, sync objects, ...) by
    // this ID rather than by pointer or by a recyclable slot index, so a stale ID can never silently
    // alias a different window.
    enum class WindowId : usize {};

    // Sentinel `WindowId` that `allocate_window_id()` never produces (ids count up from `0`). Use it
    // to mark an empty "no window" slot; it compares unequal to every real id for the life of the
    // process.
    inline constexpr WindowId invalid_window_id = static_cast<WindowId>(static_cast<usize>(~usize{0}));

    namespace Detail {

        // Hands out the next process-unique `WindowId`. **Monotonic** and never reused, even after a
        // window is destroyed; the function-local `static` gives one shared counter across every
        // translation unit that imports this partition. Thread-safe via a relaxed atomic fetch-add.
        [[nodiscard]] inline WindowId allocate_window_id() noexcept {
            static std::atomic<usize> next_id{0};
            return static_cast<WindowId>(next_id.fetch_add(1, std::memory_order_relaxed));
        }

    } // namespace Detail

    // The windowing **abstraction seam**. Every backend (SDL3 and GLFW today) derives from this, and
    // the rest of the engine speaks only this interface plus the API-agnostic types in `WindowEvent` /
    // `WindowGeometry` / `WindowConfig` — no SDL or GLFW symbols ever leak upward.
    //
    // A `Window` owns exactly one native OS window. It is **non-copyable and non-movable** (its address
    // and stable `WindowId` are its identity, and backends store per-window resources keyed by that ID)
    // and is always heap-allocated through the `create()` / `recreate()` factories.
    //
    // **Threading:** a `Window` is main-thread-affine. Drive `pump_events()` and the event/state queries
    // from the thread that owns the platform message pump. Backends may lock internally to guard
    // teardown races, but do not operate one window from multiple threads.
    //
    // ### Typical use
    // ```cpp
    // auto created = Window::create<GLFW::GLFWWindow>(
    //     WindowConfig{ .title = "Sturdy", .extent = {1280, 720} });
    // if (!created) return created.error();
    // unique_ptr<Window> window = std::move(*created);
    //
    // while (!window->close_requested()) {
    //     if (auto ok = window->pump_events(); !ok) break;          // drain OS events once
    //     while (auto ev = window->poll_event()) dispatch(*ev);      // consume them
    //     if (auto resize = window->consume_resize())               // react to the newest size
    //         renderer.rebuild_swapchain(*resize);
    //     renderer.draw_frame();
    // }
    // ```
    class Window {
      protected:
        // Construction **passkey**. Only `Window::create()` / `recreate()` can mint one, so a concrete
        // backend's `construct()` can be `public` (the templated factory has to call it) while staying
        // impossible to invoke without going through the factory.
        struct ConstructorKey {
          private:
            friend class Window;
            constexpr ConstructorKey() = default;
        };

        // Base-subobject initializer: stamps this window with its process-unique, never-reused
        // `WindowId`. Reachable only by a concrete backend holding a `ConstructorKey`, i.e. via
        // `create()` / `recreate()`.
        explicit Window(ConstructorKey) noexcept
            : id_(Detail::allocate_window_id()) {}

      public:
        // Virtual so deleting through a `unique_ptr<Window>` runs the concrete backend's destructor,
        // which releases the native window and — for the last window on a backend — tears the library
        // down.
        virtual ~Window() noexcept = default;

        // **Non-copyable and non-movable.** A window's address and `WindowId` are its identity, and
        // backends hold per-window resources keyed to it. Always pass one around by its owning
        // `unique_ptr<Window>`.
        Window(const Window &) = delete;
        Window &operator=(const Window &) = delete;
        Window(Window &&) = delete;
        Window &operator=(Window &&) = delete;

        // This window's stable identity, used to key its resources in the engine backend.
        //
        // @returns the process-unique `WindowId` assigned at construction.
        [[nodiscard]] WindowId id() const noexcept { return id_; }

        // The **sole entry point** for constructing a window. Instantiates the requested concrete
        // `Backend` through its `construct()` (gated by `ConstructorKey`, forwarding `args` to the real
        // constructor) and converts any throwing failure into a `WindowError`, so callers always get a
        // uniform `expected<>` and never see an exception escape.
        //
        // @tparam Backend concrete `Window` subclass to build (e.g. `GLFW::GLFWWindow`).
        // @param  args    forwarded to `Backend::construct` after the key — usually a `WindowConfig`.
        // @returns the owned window on success, or a `WindowError` (`OutOfMemory` / `CreationFailed`).
        //
        // ```cpp
        // auto win = Window::create<GLFW::GLFWWindow>(WindowConfig{ .title = "Editor" });
        // if (!win) log_error("window creation failed: {}", win.error().message);
        // ```
        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> create(Args &&...args) noexcept {
            try {
                return Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            } catch (const bad_alloc &) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory while creating window."});
            } catch (...) {
                return unexpected(WindowError{WindowErrorCode::CreationFailed, "Unexpected exception while creating window."});
            }
        }

        // Destroy `existing` and build a replacement **in that order**, for changes that no backend can
        // apply to a live native window — graphics API, GL/GLX pixel format, GPU-exclusive fullscreen, a
        // Wayland surface-role change, and the like.
        //
        // The strict ordering matters: `existing` is released first (freeing its native handle and, for
        // the last window on a backend, deinitializing the windowing library) **before** the replacement
        // is constructed, so the two never coexist and platform-singleton state (GLFW/SDL global init)
        // transitions cleanly even when recreating the only window.
        //
        // @warning The result has a **new** `WindowId`. Any backend resources keyed by the old id must be
        //          destroyed and rebuilt for the new window (see `Engine::recreate_window()`).
        //
        // ```cpp
        // // Graphics-API changes can't be applied live, so destroy + recreate in one step.
        // auto next = Window::recreate<GLFW::GLFWWindow>(std::move(window),
        //     WindowConfig{ .graphics_api = WindowGraphicsApi::Vulkan });
        // if (next) window = std::move(*next);  // `window` now holds a fresh WindowId
        // ```
        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> recreate(unique_ptr<Window> existing, Args &&...args) noexcept {
            existing.reset();
            return create<Backend>(std::forward<Args>(args)...);
        }

        // Which windowing library (`SDL3` or `GLFW`) backs this window. Switch on it only when a
        // library-specific path is unavoidable, e.g. picking the matching Vulkan surface provider.
        [[nodiscard]] virtual WindowBackendKind backend_kind() const noexcept = 0;

        // The `WindowingSystem` this window belongs to. Today this mirrors `backend_kind()` (each backend
        // reports its own library identity), with `WindowingSystem::Unknown` reserved for the
        // undeterminable case.
        [[nodiscard]] virtual WindowingSystem type() const noexcept = 0;

        // The windowing library's **own** handle — `SDL_Window*` or `GLFWwindow*`, type-erased as
        // `void*`. Use it to call into SDL/GLFW directly for something the abstraction doesn't cover.
        //
        // @returns the library handle, or a `WindowError` if the underlying window is already destroyed.
        [[nodiscard]] virtual expected<void *, WindowError> native_backend_handle() const noexcept = 0;

        // The **OS-native** handle bundle needed for platform integration such as Vulkan WSI and native
        // window effects: a `NativeWindowSystem` tag plus the raw `display` / `window` pointers (a Win32
        // `HWND`, an X11 `Display` + `Window`, a Wayland surface, ...).
        //
        // @returns a `NativeWindowHandle`, or a `WindowError` if the window has been destroyed.
        [[nodiscard]] virtual expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept = 0;

        // Drain the OS/backend event queue **once**, translating native events into this window's own
        // queue and updating latched state (`close_requested()`, `resized()`). Call it on the message-pump
        // thread, then pair it with `poll_event()` to read what it produced.
        //
        // @returns `{}` on success, or a `WindowError` if the backend's event pump itself fails.
        virtual expected<void, WindowError> pump_events() noexcept = 0;

        // Pop the next translated `WindowEvent`, or `nullopt` once the queue built by the last
        // `pump_events()` is drained. Call it repeatedly after each `pump_events()`:
        //
        // ```cpp
        // window->pump_events();
        // while (auto ev = window->poll_event()) dispatch(*ev);
        // ```
        [[nodiscard]] virtual optional<WindowEvent> poll_event() noexcept = 0;

        // Whether a close has been requested — by the user (window close button / OS quit) or via
        // `request_close()`. The flag **latches** and never itself destroys the window; the owner decides
        // when to tear it down (typically the `while (!close_requested())` loop condition).
        [[nodiscard]] virtual bool close_requested() const noexcept = 0;

        // Programmatically raise the close-requested flag, exactly as if the user had asked to close.
        // Does **not** destroy the window on its own.
        virtual void request_close() noexcept = 0;

        // Whether a size change is pending since the last `consume_resize()`. Cheap to poll every frame;
        // read the actual new extent with `consume_resize()`.
        [[nodiscard]] virtual bool resized() const noexcept = 0;

        // Return the newest pending framebuffer size **once** and clear the pending flag, or `nullopt` if
        // nothing is pending. A burst of resize events collapses into a single up-to-date extent, so a
        // renderer rebuilds its swapchain once instead of per intermediate size.
        //
        // ```cpp
        // if (auto resize = window->consume_resize())
        //     renderer.rebuild_swapchain(*resize);
        // ```
        [[nodiscard]] virtual optional<WindowResize> consume_resize() noexcept = 0;

        // Make the window visible.
        //
        // @note Like every state request below, this is *submitted* to the OS and may be applied
        //       asynchronously or ignored by a window manager that won't honor it — success means "the
        //       request was submitted," not "the state is now in effect."
        virtual expected<void, WindowError> show() noexcept = 0;

        // Hide the window without destroying it (bring it back with `show()`).
        virtual expected<void, WindowError> hide() noexcept = 0;

        // Ask the window manager to give this window input focus.
        virtual expected<void, WindowError> focus() noexcept = 0;

        // Raise the window above its siblings in the z-order.
        virtual expected<void, WindowError> raise() noexcept = 0;

        // Maximize the window to fill the available work area (still windowed, **not** fullscreen).
        virtual expected<void, WindowError> maximize() noexcept = 0;

        // Minimize (iconify) the window to the taskbar/dock.
        virtual expected<void, WindowError> minimize() noexcept = 0;

        // Restore the window from a maximized or minimized state back to its normal size and position.
        virtual expected<void, WindowError> restore() noexcept = 0;

        // Set the title-bar text (UTF-8).
        virtual expected<void, WindowError> set_title(const char *title) noexcept = 0;

        // The window's top-left `WindowPosition` in **logical/screen** coordinates.
        [[nodiscard]] virtual expected<WindowPosition, WindowError> position() const noexcept = 0;

        // Move the window so its top-left is at `position`, in **logical/screen** coordinates.
        virtual expected<void, WindowError> set_position(WindowPosition position) noexcept = 0;

        // The window's client-area size in **logical/screen** coordinates.
        //
        // @note On HiDPI / fractional-scaling displays this differs from `framebuffer_size()` — use
        //       `framebuffer_size()` for anything you render.
        [[nodiscard]] virtual expected<WindowExtent, WindowError> size() const noexcept = 0;

        // Resize the client area to `extent`, in **logical/screen** coordinates.
        virtual expected<void, WindowError> set_size(WindowExtent extent) noexcept = 0;

        // The drawable size in **physical pixels** — the extent the renderer must size its swapchain to.
        // Differs from `size()` whenever the display applies HiDPI / fractional scaling.
        [[nodiscard]] virtual expected<WindowExtent, WindowError> framebuffer_size() const noexcept = 0;

        // Clamp the **smallest** size the user can interactively resize the window to.
        virtual expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept = 0;

        // Clamp the **largest** size the user can interactively resize the window to.
        virtual expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept = 0;

        // Enable or disable interactive (user-drag) resizing.
        virtual expected<void, WindowError> set_resizable(bool enabled) noexcept = 0;

        // Show or hide the window's title bar and border (decorations).
        virtual expected<void, WindowError> set_decorated(bool enabled) noexcept = 0;

        // Switch between windowed, borderless-fullscreen, and exclusive-fullscreen via `WindowMode`.
        //
        // @note Changes no backend can apply to a live window (e.g. an exclusive-fullscreen pixel-format
        //       change) must go through `recreate()` instead.
        virtual expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept = 0;

        // Set whole-window opacity in `[0, 1]` (`1` = opaque), where the platform supports it.
        virtual expected<void, WindowError> set_opacity(f32 opacity) noexcept = 0;

        // The window's current whole-window opacity in `[0, 1]`.
        [[nodiscard]] virtual expected<f32, WindowError> opacity() const noexcept = 0;

        // Show or hide the mouse cursor while it is over this window.
        virtual expected<void, WindowError> set_cursor_visible(bool visible) noexcept = 0;

        // Confine the cursor to the window's bounds (it stays visible and can move within them).
        virtual expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept = 0;

        // Switch to **relative** (delta-only) mouse reporting with a hidden, recentered cursor — the mode
        // wanted for FPS-style camera control. Distinct from `set_mouse_locked()`, the combined toggle
        // below.
        virtual expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept = 0;

        // Combined **"capture the mouse for this window"** toggle, implemented on top of cursor
        // grab / visibility / relative mode. Prefer the `lock_mouse_to_window()` / `unlock_mouse()`
        // wrappers below for readability at call sites.
        virtual expected<void, WindowError> set_mouse_locked(bool locked) noexcept = 0;

        // Whether the mouse is currently locked to this window (the state set by `set_mouse_locked()`).
        [[nodiscard]] virtual bool mouse_locked() const noexcept = 0;

        // Readable wrapper for `set_mouse_locked(true)` — enter mouselook.
        //
        // ```cpp
        // window->lock_mouse_to_window();   // gameplay: capture the pointer
        // // ...
        // window->unlock_mouse();           // menu: release it
        // ```
        expected<void, WindowError> lock_mouse_to_window() noexcept {
            return set_mouse_locked(true);
        }

        // Readable wrapper for `set_mouse_locked(false)` — release the pointer.
        expected<void, WindowError> unlock_mouse() noexcept {
            return set_mouse_locked(false);
        }

        // Apply a compositor window effect — blur-behind, acrylic/mica material, dark-mode title bar,
        // custom border/caption color, ... (build one with the `WindowEffect::*` factories).
        //
        // This is the **capability-aware** primitive: its `WindowEffectResult` distinguishes full
        // `Success`, `Degraded` (applied via a fallback path), and `Failed`. Prefer it when the caller
        // wants to know *how well* the effect took.
        //
        // ```cpp
        // auto res = window->enable_window_effect(WindowEffect::mica());
        // if (res.kind == WindowEffectResultKind::Degraded)
        //     log_info("mica fell back: {}", res.details);
        // ```
        [[nodiscard]] virtual WindowEffectResult enable_window_effect(WindowEffect effect) noexcept = 0;

        // Convenience form of `enable_window_effect()` that collapses the `WindowEffectResult` into the
        // uniform `expected<void, WindowError>` (a `Degraded` outcome still counts as success). Use it for
        // fire-and-forget: `window->set_effect(WindowEffect::acrylic());`
        virtual expected<void, WindowError> set_effect(WindowEffect effect) noexcept = 0;

        // Shortcut for the common blur-behind toggle — equivalent to `set_effect(WindowEffect::blur(enabled))`.
        virtual expected<void, WindowError> set_blur_enabled(bool enabled) noexcept = 0;

        // The Vulkan **instance** extension strings this windowing backend requires (e.g.
        // `VK_KHR_surface` + the platform WSI extension).
        //
        // @warning Call **after** window creation but **before** the renderer backend initializes: these
        //          extensions are baked into the `VkInstance` at creation time. Both SDL3 and GLFW return
        //          pointers into their own static storage, valid for the backend's lifetime.
        //
        // ```cpp
        // auto exts = window->required_vulkan_instance_extensions();
        // if (!exts) return exts.error();
        // instance_info.enabledExtensionCount   = static_cast<uint32_t>(exts->size());
        // instance_info.ppEnabledExtensionNames = exts->data();
        // ```
        [[nodiscard]] virtual expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept = 0;

        // Register a callback the backend invokes to render a frame **while the OS holds the message pump
        // hostage** — most notably the Windows move/resize modal loop, during which `pump_events()` would
        // otherwise block and freeze rendering.
        //
        // The callback fires on the **main thread** from inside the blocked pump, so it must **not** call
        // back into `pump_events()`, but may otherwise call any engine or window API. Pass an empty
        // `std::function` to clear a previously registered callback. Backends without a blocking
        // modal-loop problem may leave this a no-op.
        //
        // ```cpp
        // window->set_repaint_callback([&] { engine.render(surface, frame); });
        // // ... later:
        // window->set_repaint_callback({});   // clear it
        // ```
        virtual void set_repaint_callback(std::function<void()> /*callback*/) noexcept {}

      private:
        WindowId id_;
    };

} // namespace SFT::Platform::Windowing
