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
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>
#include <WindowManager/WindowGeometry.hpp>
#include <WindowManager/WindowEvent.hpp>
#include <WindowManager/WindowEffect.hpp>
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
                return Backend::construct(ConstructorKey{}, std::forward<Args>(args)...);
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


        /// Returns the size for this `Window`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual expected<WindowExtent, WindowError> size() const noexcept = 0;


        /// Sets the size for this `Window`.
        ///
        /// @param extent `extent` value used by the operation.
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


        /// Sets the minimum size for this `Window`.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        virtual expected<void, WindowError> set_minimum_size(WindowExtent extent) noexcept = 0;


        /// Sets the maximum size for this `Window`.
        ///
        /// @param extent `extent` value used by the operation.
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

      private:
        WindowId id_;
    };

} // namespace SFT::WindowManager
