#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Async/src/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <optional>
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
        Platform::Windowing::WindowConfig config{};

        /// Constructs a `OwnedWindowConfig` in its default state.
        ///
        /// @note This function does not throw exceptions.
        OwnedWindowConfig() noexcept;
        /// Constructs a `OwnedWindowConfig` from the supplied initialization values.
        ///
        /// @param source Source value or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit OwnedWindowConfig(const Platform::Windowing::WindowConfig &source);

        /// Returns the current or globally available view value.
        ///
        /// @return Returns the current view value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Platform::Windowing::WindowConfig view() const noexcept;
    };

    struct SpawnWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};

        Platform::Windowing::WindowFactory factory = nullptr;
    };


    struct RecreatePrimaryWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};
        Platform::Windowing::WindowFactory factory = nullptr;
    };

    struct CloseWindowRequest {
        WindowRequestId id{};
        Platform::Windowing::WindowId window{};
    };


    struct SetCursorIconRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::CursorIcon icon = Platform::Windowing::CursorIcon::Default;
    };


    struct SetFullscreenRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowMode mode = Platform::Windowing::WindowMode::Windowed;
    };

    struct SetDecoratedRequest {
        Platform::Windowing::WindowId window{};
        bool decorated = true;
    };


    struct SetTransparentRequest {
        Platform::Windowing::WindowId window{};
        bool transparent = false;
    };


    struct SetBlurRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowEffectKind kind = Platform::Windowing::WindowEffectKind::Blur;
        bool enabled = false;
    };


    struct SetTextInputAreaRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::TextInputArea area{};
    };


    struct SetTextInputActiveRequest {
        Platform::Windowing::WindowId window{};
        bool active = true;
    };

    using WindowRequest = variant<SpawnWindowRequest, CloseWindowRequest, RecreatePrimaryWindowRequest,
                                  SetCursorIconRequest, SetFullscreenRequest, SetDecoratedRequest,
                                  SetTransparentRequest, SetBlurRequest, SetTextInputAreaRequest,
                                  SetTextInputActiveRequest>;

    enum class WindowRequestKind : u8 { Spawn, Close, RecreatePrimary };

    struct WindowRequestCompletion {
        WindowRequestId id{};
        WindowRequestKind kind = WindowRequestKind::Spawn;
        bool accepted = false;
        optional<Core::RenderSurfaceHandle> surface;
        Platform::Windowing::WindowId window{};
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
        [[nodiscard]] WindowRequestId spawn(const Platform::Windowing::WindowConfig &config,
                                            Platform::Windowing::WindowFactory factory = nullptr);

        /// Closes the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowRequestId close(Platform::Windowing::WindowId window);

        /// Recreates primary window using the supplied arguments and current state.
        ///
        /// @param config Configuration values controlling the operation.
        /// @param factory `factory` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] WindowRequestId recreate_primary_window(const Platform::Windowing::WindowConfig &config,
                                                               Platform::Windowing::WindowFactory factory = nullptr);


        /// Sets the cursor icon for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param icon `icon` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_cursor_icon(Platform::Windowing::WindowId window, Platform::Windowing::CursorIcon icon);


        /// Sets the fullscreen for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_fullscreen(Platform::Windowing::WindowId window, Platform::Windowing::WindowMode mode);


        /// Sets the decorated for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param decorated `decorated` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_decorated(Platform::Windowing::WindowId window, bool decorated);

        /// Sets the transparent for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param transparent `transparent` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_transparent(Platform::Windowing::WindowId window, bool transparent);

        /// Sets the blur for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_blur(Platform::Windowing::WindowId window, Platform::Windowing::WindowEffectKind kind, bool enabled);


        /// Sets the text input area for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param area `area` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_text_input_area(Platform::Windowing::WindowId window, Platform::Windowing::TextInputArea area);

        /// Sets the text input active for this `WindowRequests`.
        ///
        /// @param window Window used or affected by the operation.
        /// @param active `active` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void set_text_input_active(Platform::Windowing::WindowId window, bool active);

        /// Drains the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current drain value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
        };
        mutable Async::Mutex<State> state_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowRequests, "sturdy.engine.window_requests");
