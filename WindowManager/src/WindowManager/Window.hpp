#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <concepts>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowGeometry.hpp>
#include <WindowManager/WindowEvent.hpp>
#include <WindowManager/WindowEffect.hpp>
#include <WindowManager/WindowChrome.hpp>
#include <WindowManager/WindowConfig.hpp>

using std::bad_alloc;
using std::derived_from;
using std::expected;
using std::optional;
using std::unexpected;
using std::unique_ptr;
using std::vector;

namespace SFT::WindowManager {


    enum class WindowingSystem {
        Unknown,
        SDL3,
        GLFW,
    };


    enum class WindowId : usize {};


    struct WindowHdrProperties {
        bool hdr_enabled = false;
        f32 sdr_white_level = 1.0f;
        f32 hdr_headroom = 1.0f;
    };


    inline constexpr WindowId invalid_window_id = static_cast<WindowId>(static_cast<usize>(~usize{0}));


    enum class CursorIcon : u8 {
        Default,
        Pointer,
        Text,
        Grab,
        Grabbing,
        ResizeHorizontal,
        ResizeVertical,
        ResizeNwse,
        ResizeNesw,
        NotAllowed,
        Crosshair,
        /// Four-way move / resize-all.
        Move,
        /// Busy (the program cannot take input); not available on every provider.
        Wait,
        /// Busy but still interactive (arrow with a spinner).
        Progress,
        /// Edge and corner resize arrows. A provider with only the axis cursors shows the matching axis one.
        ResizeN,
        ResizeNe,
        ResizeE,
        ResizeSe,
        ResizeS,
        ResizeSw,
        ResizeW,
        ResizeNw,
    };

    namespace Detail {


        /// Allocates window ID.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowId allocate_window_id() noexcept;

    } // namespace Detail


    class Window {
      protected:


        struct ConstructorKey {
          private:
            friend class Window;
            /// Constructs a `ConstructorKey` in its default state.
            ///
            /// @note This function does not throw exceptions.
            constexpr ConstructorKey() = default;
        };


        /// Constructs a `Window` from the supplied initialization values.
        ///
        /// @note This function does not throw exceptions.
        explicit Window(ConstructorKey) noexcept;

      public:


        /// Destroys the `Window` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~Window() noexcept = default;


        /// Disables this construction form for `Window`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Window(const Window &) = delete;
        /// Assigns a new value to this `Window`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Window &operator=(const Window &) = delete;
        /// Disables this construction form for `Window`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Window(Window &&) = delete;
        /// Assigns a new value to this `Window`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Window &operator=(Window &&) = delete;


        /// Returns the current or globally available ID value.
        ///
        /// @return Returns the current ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowId id() const noexcept;


        /// Constructs the requested concrete window-backend type from the supplied arguments.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Returns `WindowErrorCode::OutOfMemory` if backend construction throws `std::bad_alloc`; other construction exceptions are converted to `WindowErrorCode::CreationFailed`.
        /// @note This function does not throw exceptions.
        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> create(Args &&...args) noexcept {
            try {
                // The creation-time behaviour in the config is applied once the window exists.
                const WindowConfig *config = nullptr;
                ((config = config != nullptr ? config : config_of(args)), ...);
                auto created = Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
                if (created.has_value() && config != nullptr) {
                    (*created)->apply_creation_config(*config);
                }
                return created;
            } catch (const bad_alloc &) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory while creating window."});
            } catch (...) {
                return unexpected(WindowError{WindowErrorCode::CreationFailed, "Unexpected exception while creating window."});
            }
        }


        /// Recreates the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Returns `WindowErrorCode::OutOfMemory` if backend construction throws `std::bad_alloc`; other construction exceptions are converted to `WindowErrorCode::CreationFailed`.
        /// @note This function does not throw exceptions.
        template <typename Backend, typename... Args>
            requires derived_from<Backend, Window> && requires(Args &&...args) {
                Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static expected<unique_ptr<Backend>, WindowError> recreate(unique_ptr<Window> existing, Args &&...args) noexcept {
            existing.reset();
            return create<Backend>(std::forward<Args>(args)...);
        }


        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual WindowBackendKind backend_kind() const noexcept = 0;


        /// Returns the runtime or backend type represented by `Window`.
        ///
        /// @return Returns the current type value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual WindowingSystem type() const noexcept = 0;


        /// Returns the native backend handle associated with this `Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<void *, WindowError> native_backend_handle() const noexcept = 0;


        /// Returns the native window handle associated with this `Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept = 0;


        /// Returns the current or globally available HDR properties value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual optional<WindowHdrProperties> hdr_properties() const noexcept;


        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> pump_events() noexcept = 0;


        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual optional<WindowEvent> poll_event() noexcept = 0;


        /// Closes requested using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual bool close_requested() const noexcept = 0;


        /// Requests close using the supplied arguments and current state.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void request_close() noexcept = 0;


        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual bool resized() const noexcept = 0;


        /// Returns the current or globally available consume resize value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual optional<WindowResize> consume_resize() noexcept = 0;


        /// Returns the current or globally available show value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> show() noexcept = 0;


        /// Returns the current or globally available hide value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> hide() noexcept = 0;


        /// Returns the current or globally available focus value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> focus() noexcept = 0;


        /// Returns the current or globally available raise value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> raise() noexcept = 0;


        /// Returns the current or globally available maximize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> maximize() noexcept = 0;


        /// Returns the current or globally available minimize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> minimize() noexcept = 0;


        /// Returns the current or globally available restore value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> restore() noexcept = 0;


        /// Sets the title for this `Window`.
        ///
        /// @param title `title` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_title(const char *title) noexcept = 0;


        /// Returns the current or globally available position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<WindowPosition, WindowError> position() const noexcept = 0;


        /// Sets the position for this `Window`.
        ///
        /// @param position `position` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_position(WindowPosition position) noexcept = 0;


        /// Returns the current or globally available global cursor position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::Unsupported`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<WindowPosition, WindowError> global_cursor_position() const noexcept;


        /// Returns the size for this `Window` in the provider's window coordinates (physical pixels on
        /// Win32/X11, points on macOS/scaled Wayland/Web). Use `framebuffer_size()` for pixels.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<WindowExtent, WindowError> size() const noexcept = 0;


        /// Sets the size for this `Window`.
        ///
        /// @param extent Requested client-area size in physical pixels; providers convert it to their own
        ///        window coordinates using the window's current pixel density.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_size(WindowExtent extent) noexcept = 0;


        /// Returns the framebuffer size for this `Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<WindowExtent, WindowError> framebuffer_size() const noexcept = 0;


        /// Returns the window's content scale: the UI scale factor the OS asks applications to use on the
        /// display the window is on (1.0 at 100%, 2.0 at 200%). A `ContentScaleChanged` event is queued
        /// when it changes.
        ///
        /// @return The content scale, or 1.0 for providers that cannot report one.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual f32 content_scale() const noexcept;


        /// Returns the current refresh rate, in Hz, of whichever display this window is presently on.
        ///
        /// A window can move between displays with different refresh rates (a multi-monitor setup, most
        /// commonly), so this reflects "right now," not a value fixed at window creation; a caller pacing
        /// frames to it (see `Core::FrameRateLimitMode::MatchDisplayRefresh`) should re-query it periodically
        /// rather than caching it once. Real hardware occasionally reports a rate too small to trust (0, or a
        /// display that hasn't settled after a mode change) — never a negative or otherwise nonsensical one —
        /// so a value at or below zero here means "not currently available," not "the display runs at 0Hz."
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the
        ///         operation failed (no display owns this window yet, or the platform has nothing to report).
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<f32, WindowError> refresh_rate_hz() const noexcept = 0;


        /// Sets the minimum size for this `Window`.
        ///
        /// @param extent Minimum client-area size in physical pixels (see `set_size()`).
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept = 0;


        /// Sets the maximum size for this `Window`.
        ///
        /// @param extent Maximum client-area size in physical pixels (see `set_size()`).
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_maximum_size(WindowExtent extent) noexcept = 0;


        /// Sets the resizable for this `Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_resizable(bool enabled) noexcept = 0;


        /// Sets the decorated for this `Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_decorated(bool enabled) noexcept = 0;


        /// Sets the fullscreen for this `Window`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_fullscreen(WindowMode mode) noexcept = 0;


        /// Returns the current or globally available fullscreen mode value.
        ///
        /// @return Returns the current fullscreen mode value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual WindowMode fullscreen_mode() const noexcept = 0;


        /// Sets the opacity for this `Window`.
        ///
        /// @param opacity `opacity` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_opacity(f32 opacity) noexcept = 0;


        /// Returns the current or globally available opacity value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<f32, WindowError> opacity() const noexcept = 0;


        /// Sets the cursor visible for this `Window`.
        ///
        /// @param visible `visible` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_cursor_visible(bool visible) noexcept = 0;


        /// Sets the cursor icon for this `Window`.
        ///
        /// @param icon `icon` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_cursor_icon(CursorIcon icon) noexcept = 0;


        /// Sets the cursor grabbed for this `Window`.
        ///
        /// @param grabbed `grabbed` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_cursor_grabbed(bool grabbed) noexcept = 0;


        /// Sets the relative mouse mode for this `Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_relative_mouse_mode(bool enabled) noexcept = 0;


        /// Sets the mouse locked for this `Window`.
        ///
        /// @param locked `locked` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_mouse_locked(bool locked) noexcept = 0;


        /// Returns the current or globally available mouse locked value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual bool mouse_locked() const noexcept = 0;


        /// Acquires the associated synchronization primitive before protected access.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> lock_mouse_to_window() noexcept;


        /// Releases the associated synchronization primitive.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> unlock_mouse() noexcept;


        /// Enables window effect using the supplied arguments and current state.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual WindowEffectResult enable_window_effect(WindowEffect effect) noexcept = 0;

        /// What this window can do for `kind` on this OS and compositor, route by route (for example which of
        /// `ext-background-effect-v1`, `org_kde_kwin_blur` and the X11 blur property exist). Changes nothing. The
        /// default asks the platform layer through `native_window_handle()`.
        [[nodiscard]] virtual WindowEffectSupport window_effect_support(WindowEffectKind kind) const noexcept;

        // -- Custom title bar / window chrome. Defaults report `Unsupported`; the SDL3 provider implements them. --

        /// Installs the drag/resize regions of a custom (undecorated) title bar; see `HitTestRegions`.
        virtual expected<void, WindowError> set_hit_test_regions(HitTestRegions regions) noexcept;
        /// Opens the OS window menu (move/size/minimize/close) at `position` in window coordinates, as a right-click on
        /// a native title bar would. Windows and some Linux window managers.
        virtual expected<void, WindowError> show_system_menu(WindowPosition position) noexcept;
        [[nodiscard]] virtual bool maximized() const noexcept;
        [[nodiscard]] virtual bool minimized() const noexcept;
        /// Window icon from tightly packed RGBA8 pixels (`width * height * 4` bytes). On Wayland the compositor uses the
        /// desktop file matching `WindowConfig::app_id` instead where it does not support per-window icons.
        virtual expected<void, WindowError> set_icon(u32 width, u32 height, std::span<const std::byte> rgba) noexcept;
        virtual expected<void, WindowError> request_attention(AttentionRequest request) noexcept;
        /// Taskbar/dock progress. `value` is 0..1 and only used by `Normal`, `Paused` and `Error`.
        virtual expected<void, WindowError> set_taskbar_progress(TaskbarProgress state, f32 value) noexcept;
        /// The OS light/dark preference (`WindowEventKind::SystemThemeChanged` reports changes).
        [[nodiscard]] virtual SystemTheme system_theme() const noexcept;
        /// Every connected monitor.
        [[nodiscard]] virtual std::vector<DisplayInfo> displays() const noexcept;


        /// Sets the effect for this `Window`.
        ///
        /// @param effect `effect` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_effect(WindowEffect effect) noexcept = 0;


        /// Sets the blur enabled for this `Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_blur_enabled(bool enabled) noexcept = 0;


        /// Sets the transparent for this `Window`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_transparent(bool enabled) noexcept = 0;


        /// Returns the current or globally available required vulkan instance extensions value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<vector<const char *>, WindowError>
        required_vulkan_instance_extensions() const noexcept = 0;


        /// Creates a vulkan surface from the supplied parameters.
        ///
        /// @param instance Instance used or affected by the operation.
        /// @param allocation_callbacks Callable invoked by the operation.
        /// @param surface_out Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> create_vulkan_surface(
            void *instance,
            const void *allocation_callbacks,
            void *surface_out) const noexcept = 0;


        /// Sets the live resize callback for this `Window`.
        ///
        /// @note This function does not throw exceptions.
        virtual void set_live_resize_callback(std::function<void(WindowExtent)>             ) noexcept;


        /// Returns the current or globally available clipboard text value.
        ///
        /// @return Returns the current clipboard text value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual std::string clipboard_text() const noexcept = 0;


        /// Sets the clipboard text for this `Window`.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_clipboard_text(std::string_view text) noexcept = 0;

        /// Blocks the calling thread (the one that pumps this window's events) until the platform has an event for the
        /// process, `wake_native_event_wait` is called, or `timeout` passes. Returns false at once when the provider has
        /// no blocking wait, in which case the caller polls instead. Events are left queued for `pump_events`.
        [[nodiscard]] virtual bool wait_for_native_events(std::chrono::microseconds timeout) noexcept;

        /// Interrupts a `wait_for_native_events` in progress or about to start, from any thread (an event wait entered
        /// afterwards returns at once). Must not touch `this`: the returned function is cached by the window manager and
        /// may run after the window is gone. Null when the provider has no blocking wait.
        [[nodiscard]] virtual void (*native_event_waker() const noexcept)() noexcept;

        /// True when the clipboard currently offers `mime_type` (e.g. "image/png", "text/uri-list").
        ///
        /// Backends without typed clipboard support report false.
        [[nodiscard]] virtual bool clipboard_has_mime(std::string_view mime_type) const noexcept;

        /// Reads the clipboard payload offered under `mime_type`; empty when absent or unsupported.
        [[nodiscard]] virtual std::vector<std::byte> clipboard_data(std::string_view mime_type) const noexcept;

        /// Replaces the clipboard with one payload under `mime_type` (e.g. PNG bytes under "image/png", or
        /// newline-separated `file://` URIs under "text/uri-list"). The bytes are copied.
        virtual expected<void, WindowError> set_clipboard_data(std::string_view mime_type,
                                                               std::span<const std::byte> bytes) noexcept;


        /// Starts text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> start_text_input() noexcept;


        /// Stops text input using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> stop_text_input() noexcept;


        /// Sets the text input area for this `Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_text_input_area(TextInputArea         ) noexcept;

        // -- The union surface. Everything any provider can do is a method here (see WindowFeatures.hpp). Defaults
        //    report `Unsupported`, or build on the native handle where that works for every provider; a provider
        //    overrides what its host library does better. `feature_support` says which is which. --

        /// Whether, and how fully, this window supports `feature` on this provider, OS and session type.
        [[nodiscard]] virtual FeatureSupport feature_support(WindowFeature feature) const noexcept;

        virtual expected<void, WindowError> set_always_on_top(bool enabled) noexcept;
        /// Constrains resizing to width/height ratios in [`min_ratio`, `max_ratio`]; 0 and 0 remove the constraint. A
        /// provider that only supports one exact ratio rejects `min_ratio != max_ratio` as `Unsupported`.
        virtual expected<void, WindowError> set_aspect_ratio(f32 min_ratio, f32 max_ratio) noexcept;
        /// Mouse clicks fall through to what is behind the window. Built on the native handle for every provider.
        virtual expected<void, WindowError> set_mouse_passthrough(bool enabled) noexcept;
        /// A non-focusable window never takes keyboard focus (palettes, overlays).
        virtual expected<void, WindowError> set_focusable(bool enabled) noexcept;
        /// Fullscreen windows minimise when they lose focus (the default for exclusive fullscreen on most systems).
        virtual expected<void, WindowError> set_auto_minimize_on_focus_loss(bool enabled) noexcept;
        /// Makes this window a child of `parent` (null removes the relationship). A modal child blocks its parent.
        virtual expected<void, WindowError> set_parent(Window *parent, bool modal) noexcept;
        /// Routes all keyboard input to this window, including window-manager shortcuts. Built on the native handle.
        virtual expected<void, WindowError> set_keyboard_grab(bool enabled) noexcept;
        /// Confines the cursor to a rectangle of the client area (window coordinates); empty releases it.
        virtual expected<void, WindowError> set_mouse_confine_rect(std::optional<WindowRect> rect) noexcept;
        /// Moves the cursor to `position` (window coordinates).
        virtual expected<void, WindowError> set_cursor_position(WindowPosition position) noexcept;
        /// Shows a cursor drawn from an image in place of the `set_cursor_icon` shape until `clear_custom_cursor`.
        virtual expected<void, WindowError> set_custom_cursor(const CursorImage &image) noexcept;
        virtual expected<void, WindowError> clear_custom_cursor() noexcept;
        /// The decoration thickness around the client area.
        [[nodiscard]] virtual expected<FrameExtents, WindowError> frame_extents() const noexcept;
        /// The part of the client area not covered by notches, rounded corners or system bars. The whole client area on
        /// desktops.
        [[nodiscard]] virtual expected<WindowRect, WindowError> safe_area() const noexcept;
        /// `start_text_input` telling the IME / on-screen keyboard what kind of text is being entered.
        virtual expected<void, WindowError> start_text_input_with(const TextInputOptions &options) noexcept;
        [[nodiscard]] virtual bool has_screen_keyboard() const noexcept;
        [[nodiscard]] virtual bool screen_keyboard_shown() const noexcept;
        /// The clipboard formats currently available (always includes a text type when text is present).
        [[nodiscard]] virtual std::vector<std::string> clipboard_mime_types() const noexcept;
        /// The display and mode `set_fullscreen` uses from now on; applies immediately when already fullscreen.
        virtual expected<void, WindowError> set_fullscreen_target(FullscreenTarget target) noexcept;
        /// Tells the window manager what the window is for (palette, tooltip, popup menu, dialog, splash screen). Built
        /// on the native handle for every provider; best set before the window is first shown.
        virtual expected<void, WindowError> set_window_type(WindowType type) noexcept;
        /// Gives the window a non-rectangular outline from an alpha mask. Empty coverage restores the rectangle. Built on
        /// the native handle (X11 Shape, `SetWindowRgn`); hard-edged, since the outline is a 1-bit region.
        virtual expected<void, WindowError> set_shape(const WindowShapeMask &mask) noexcept;
        /// A cursor that cycles through `frames`. A provider without animated cursors advances the frames itself.
        virtual expected<void, WindowError> set_animated_cursor(std::span<const CursorFrame> frames) noexcept;
        /// Moves the cursor to desktop coordinates.
        virtual expected<void, WindowError> set_global_cursor_position(WindowPosition position) noexcept;
        /// Keeps delivering mouse events while a button is held after the cursor leaves the window.
        virtual expected<void, WindowError> set_mouse_capture(bool enabled) noexcept;
        /// The layout's name for a key ("A", "Enter", ...); empty when the key has no name here.
        [[nodiscard]] virtual std::string key_name(KeyboardKey key) const noexcept;
        /// Whether a key is held right now.
        [[nodiscard]] virtual bool key_down(KeyboardKey key) const noexcept;
        /// The colour profile of the display the window is on (ICC bytes).
        [[nodiscard]] virtual expected<std::vector<std::byte>, WindowError> icc_profile() const noexcept;
        /// Sets the display's gamma (1.0 = unchanged) for one display, or the window's display when unset.
        virtual expected<void, WindowError> set_display_gamma(std::optional<u32> display_id, f32 gamma) noexcept;
        /// The id (`DisplayInfo::id`) of the display the window is mostly on.
        [[nodiscard]] virtual std::optional<u32> current_display_id() const noexcept;

        /// Applies the creation-time `WindowConfig` fields (always-on-top, maximized, parent, size limits, ...) through
        /// the setters above. A field a provider cannot honour is logged and skipped. Called by `create`.
        void apply_creation_config(const WindowConfig &config) noexcept;

      private:
        template <typename T>
        [[nodiscard]] static const WindowConfig *config_of(const T &value) noexcept {
            if constexpr (std::is_same_v<std::remove_cvref_t<T>, WindowConfig>) {
                return &value;
            } else {
                return nullptr;
            }
        }

        WindowId id_;
    };

} // namespace SFT::WindowManager
