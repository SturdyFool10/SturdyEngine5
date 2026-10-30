#pragma once

#include <Foundation/Foundation.hpp>

#include <Async/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/Resource.hpp>
#include <WindowManager/WindowManager.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using std::optional;
using std::variant;
using std::vector;

namespace SFT::Engine {

    struct WindowRequestId {
        u64 value = 0;
        /// Converts the `WindowRequestId` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
        /// Compares the operands and produces their ordering.
        ///
        /// @return Returns the comparison category describing the ordering of the operands.
        /// @note This function does not throw exceptions.
        [[nodiscard]] auto operator<=>(const WindowRequestId &) const = default;
    };


    struct OwnedWindowConfig {
        UString title{"Sturdy Engine"};
        WindowManager::WindowConfig config{};

        /// Constructs a `OwnedWindowConfig` in its default state.
        ///
        /// @note This function does not throw exceptions.
        OwnedWindowConfig() noexcept;
        /// Constructs a `OwnedWindowConfig` from the supplied initialization values.
        ///
        /// @param source Source value or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit OwnedWindowConfig(const WindowManager::WindowConfig &source);

        /// Returns the current or globally available view value.
        ///
        /// @return Returns the current view value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowManager::WindowConfig view() const noexcept;
    };

    struct SpawnWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};

        WindowManager::WindowFactory factory = nullptr;
        /// Set by `spawn_child`: made the child of this window as soon as it exists.
        std::optional<WindowManager::WindowId> parent;
        bool parent_modal = false;
    };


    struct RecreatePrimaryWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};
        WindowManager::WindowFactory factory = nullptr;
    };

    struct CloseWindowRequest {
        WindowRequestId id{};
        WindowManager::WindowId window{};
    };


    struct SetCursorIconRequest {
        WindowManager::WindowId window{};
        WindowManager::CursorIcon icon = WindowManager::CursorIcon::Default;
    };


    struct SetFullscreenRequest {
        WindowManager::WindowId window{};
        WindowManager::WindowMode mode = WindowManager::WindowMode::Windowed;
    };

    struct SetDecoratedRequest {
        WindowManager::WindowId window{};
        bool decorated = true;
    };


    struct SetTransparentRequest {
        WindowManager::WindowId window{};
        bool transparent = false;
    };


    struct SetRelativeMouseModeRequest {
        WindowManager::WindowId window{};
        bool enabled = false;
    };


    struct SetMouseLockedRequest {
        WindowManager::WindowId window{};
        bool locked = false;
    };


    struct SetCursorGrabbedRequest {
        WindowManager::WindowId window{};
        bool grabbed = false;
    };


    struct SetBlurRequest {
        WindowManager::WindowId window{};
        WindowManager::WindowEffectKind kind = WindowManager::WindowEffectKind::Blur;
        bool enabled = false;
        /// Linux only: which compositor blur protocol to use (`Automatic` tries ext-background-effect, then KDE).
        WindowManager::LinuxBlurProtocol linux_blur_protocol = WindowManager::LinuxBlurProtocol::Automatic;
        /// ARGB colour for the colour effects (`BorderColor`, `CaptionColor`, `TextColor`).
        u32 color_argb = 0;
        /// Per-effect settings (blur region, corner radius, accent tint, macOS material, ...).
        WindowManager::WindowEffectOptions options{};
    };

    /// What the platform actually did with the last effect request for one window and kind. Written on the window
    /// thread when the request runs, so it lags the request by up to a frame.
    struct WindowEffectStatus {
        WindowManager::WindowId window{};
        WindowManager::WindowEffectKind kind = WindowManager::WindowEffectKind::Blur;
        /// What the app asked for (enable or disable).
        bool requested_enabled = false;
        WindowManager::WindowEffectResultKind result = WindowManager::WindowEffectResultKind::Failed;
        /// Platform-provided detail ("The Wayland compositor does not advertise ext-background-effect-v1.", ...).
        std::string details;
    };


    struct SetTextInputAreaRequest {
        WindowManager::WindowId window{};
        WindowManager::TextInputArea area{};
    };


    struct SetTextInputActiveRequest {
        WindowManager::WindowId window{};
        bool active = true;
    };

    /// Every window operation the generic request can run. Each is one `WindowManager::Window` method; its outcome is
    /// read back with `WindowRequests::operation_status`.
    enum class WindowOperation : u8 {
        // Basics
        Title,
        Position,
        Size,
        MinimumSize,
        MaximumSize,
        Resizable,
        Opacity,
        Maximize,
        Minimize,
        Restore,
        Raise,
        Focus,
        Show,
        Hide,
        // The union surface
        AlwaysOnTop,
        AspectRatio,
        MousePassthrough,
        Focusable,
        AutoMinimize,
        Parent,
        KeyboardGrab,
        MouseConfineRect,
        CursorPosition,
        GlobalCursorPosition,
        MouseCapture,
        CustomCursor,
        AnimatedCursor,
        ClearCustomCursor,
        TextInputOptions,
        FullscreenTarget,
        WindowType,
        Shape,
        DisplayGamma,
        Icon,
        Attention,
        TaskbarProgress,
        HitTestRegions,
        SystemMenu,
        // Queries and clipboard (their results come back in `WindowOperationStatus::output`)
        ClipboardText,
        SetClipboardText,
        ClipboardMimeTypes,
        ClipboardData,
        SetClipboardData,
        KeyName,
        KeyDown,
        IccProfile,
        // Process-wide desktop services parented to a window. The answer to a file dialog arrives through the callback it
        // was started with; a message box's pressed button is `WindowOperationOutput::number`.
        FileDialog,
        MessageBox,
        Count,
    };

    /// What an operation produced besides success: the text, bytes, list or flag a query returns.
    struct WindowOperationOutput {
        std::string text;
        std::vector<std::byte> bytes;
        std::vector<std::string> list;
        bool flag = false;
        /// A numeric answer (the button a message box returned).
        std::int64_t number = 0;
    };

    using WindowOperationFn = std::function<std::expected<void, WindowManager::WindowError>(WindowManager::Window &, WindowOperationOutput &)>;

    /// Runs `run` on the window's own thread, which is where every `Window` method must be called.
    struct RunWindowOperationRequest {
        WindowManager::WindowId window{};
        WindowOperation operation = WindowOperation::Title;
        WindowOperationFn run;
    };

    /// The outcome of the last run of one operation on one window.
    struct WindowOperationStatus {
        WindowManager::WindowId window{};
        WindowOperation operation = WindowOperation::Title;
        bool ok = false;
        /// `WindowErrorCode::Unsupported` means the provider or platform cannot do it, which is not a bug.
        WindowManager::WindowErrorCode code = WindowManager::WindowErrorCode::OperationFailed;
        std::string message;
        WindowOperationOutput output;
    };

    using WindowRequest = variant<SpawnWindowRequest, CloseWindowRequest, RecreatePrimaryWindowRequest,
                                  SetCursorIconRequest, SetFullscreenRequest, SetDecoratedRequest,
                                  SetTransparentRequest, SetBlurRequest, SetTextInputAreaRequest,
                                  SetTextInputActiveRequest, SetRelativeMouseModeRequest,
                                  SetMouseLockedRequest, SetCursorGrabbedRequest, RunWindowOperationRequest>;

    enum class WindowRequestKind : u8 { Spawn, Close, RecreatePrimary };

    struct WindowRequestCompletion {
        WindowRequestId id{};
        WindowRequestKind kind = WindowRequestKind::Spawn;
        bool accepted = false;
        optional<Core::RenderSurfaceHandle> surface;
        WindowManager::WindowId window{};
        UString message;
    };


    class WindowRequests {
      public:
        /// Spawns the supplied asynchronous work.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowRequestId spawn(const WindowManager::WindowConfig &config,
                                            WindowManager::WindowFactory factory = nullptr);

        /// `spawn` for a window that is a child of an existing window (`parent`), optionally modal. `config.parent` is
        /// ignored: a raw window pointer cannot cross threads, so the relationship is made by window id once the child
        /// exists.
        [[nodiscard]] WindowRequestId spawn_child(const WindowManager::WindowConfig &config, WindowManager::WindowId parent, bool modal,
                                                  WindowManager::WindowFactory factory = nullptr);

        /// Closes the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowRequestId close(WindowManager::WindowId window);

        /// Recreates primary window using the supplied arguments and current state.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowRequestId recreate_primary_window(const WindowManager::WindowConfig &config,
                                                               WindowManager::WindowFactory factory = nullptr);


        /// Sets the cursor icon for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param icon `icon` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_cursor_icon(WindowManager::WindowId window, WindowManager::CursorIcon icon);


        /// Sets the fullscreen for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_fullscreen(WindowManager::WindowId window, WindowManager::WindowMode mode);


        /// Sets the decorated for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param decorated `decorated` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_decorated(WindowManager::WindowId window, bool decorated);

        /// Queues a full window effect (kind, protocol, colour). The platform's verdict is read back with
        /// `effect_status` once the request has run.
        void set_effect(WindowManager::WindowId window, WindowManager::WindowEffect effect);

        /// Records the platform's verdict for an effect request (called by `Application` on the window thread).
        void record_effect_status(WindowEffectStatus status);

        /// The platform's verdict on the last request of `kind` for `window`, or `nullopt` if none has run yet.
        [[nodiscard]] std::optional<WindowEffectStatus> effect_status(WindowManager::WindowId window,
                                                                      WindowManager::WindowEffectKind kind) const;

        /// Sets the transparent for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param transparent `transparent` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_transparent(WindowManager::WindowId window, bool transparent);

        /// Sets the relative mouse mode for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_relative_mouse_mode(WindowManager::WindowId window, bool enabled);

        /// Sets the mouse locked for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param locked `locked` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_mouse_locked(WindowManager::WindowId window, bool locked);

        /// Sets the cursor grabbed for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param grabbed `grabbed` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_cursor_grabbed(WindowManager::WindowId window, bool grabbed);

        /// Sets the blur for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_blur(WindowManager::WindowId window, WindowManager::WindowEffectKind kind, bool enabled);


        /// Sets the text input area for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param area `area` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_text_input_area(WindowManager::WindowId window, WindowManager::TextInputArea area);

        /// Sets the text input active for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param active `active` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_text_input_active(WindowManager::WindowId window, bool active);

        /// Drains the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current drain value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        /// Queues `run` to execute on the window's thread; the outcome is read back with `operation_status`.
        void run(WindowManager::WindowId window, WindowOperation operation, WindowOperationFn fn);

        /// Written by `Application` on the window thread after an operation ran.
        void record_operation_status(WindowOperationStatus status);

        /// The last outcome of `operation` on `window`; empty until it has run.
        [[nodiscard]] std::optional<WindowOperationStatus> operation_status(WindowManager::WindowId window, WindowOperation operation) const;

        [[nodiscard]] vector<WindowRequest> drain();

        /// Performs the complete operation for `WindowRequests` using the supplied arguments.
        ///
        /// @param completion `completion` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void complete(WindowRequestCompletion completion);

        /// Returns the current or globally available take completions value.
        ///
        /// @return Returns the current take completions value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<WindowRequestCompletion> take_completions();

        /// Reports whether this `WindowRequests` has pending.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool has_pending() const;

      private:
        struct State {
            u64 next_id = 1;
            vector<WindowRequest> pending;
            vector<WindowRequestCompletion> completions;
            vector<WindowEffectStatus> effect_statuses;
            vector<WindowOperationStatus> operation_statuses;
        };
        mutable Async::Mutex<State> state_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowRequests, "sturdy.engine.window_requests");
