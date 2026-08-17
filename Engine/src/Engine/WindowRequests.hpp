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
        [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
        [[nodiscard]] auto operator<=>(const WindowRequestId &) const = default;
    };

    // Owns WindowConfig's otherwise-borrowed title so requests can safely wait in a queue until the
    // Application host reaches its window-owner phase.
    struct OwnedWindowConfig {
        UString title{"Sturdy Engine"};
        Platform::Windowing::WindowConfig config{};

        OwnedWindowConfig() noexcept { config.title = nullptr; }
        explicit OwnedWindowConfig(const Platform::Windowing::WindowConfig &source)
            : title(source.title != nullptr ? source.title : ""), config(source) {
            config.title = nullptr;
        }

        [[nodiscard]] Platform::Windowing::WindowConfig view() const noexcept {
            Platform::Windowing::WindowConfig result = config;
            result.title = title.c_str();
            return result;
        }
    };

    struct SpawnWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};
        // Null inherits ApplicationConfig::primary_window_factory.
        Platform::Windowing::WindowFactory factory = nullptr;
    };

    // Tears down the current primary window and spawns a replacement through `factory` (null
    // inherits ApplicationConfig::primary_window_factory, same convention as SpawnWindowRequest) —
    // e.g. switching the primary surface from Platform's built-in SDL3 provider to
    // GlfwWindowProvider (or back) at runtime. See Application::recreate_primary_window()'s own doc
    // comment for the actual teardown/promotion sequence; this struct is just the deferred request
    // shape, same "GameLogic can't reach Application directly" reasoning as SpawnWindowRequest.
    struct RecreatePrimaryWindowRequest {
        WindowRequestId id{};
        OwnedWindowConfig window{};
        Platform::Windowing::WindowFactory factory = nullptr;
    };

    struct CloseWindowRequest {
        WindowRequestId id{};
        Platform::Windowing::WindowId window{};
    };

    // Fire-and-forget, unlike Spawn/Close above — a caller driving this off per-frame hover state
    // (UI::Context::desired_cursor(), translated to Platform::Windowing::CursorIcon) has no use for
    // a WindowRequestId/completion round-trip for something that's already stale a frame later
    // anyway. Application::process_window_requests() applies it directly and moves on.
    struct SetCursorIconRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::CursorIcon icon = Platform::Windowing::CursorIcon::Default;
    };

    // The four requests below are fire-and-forget for the same reason as SetCursorIconRequest above:
    // simple window state pushes with nothing for a caller to meaningfully await.
    struct SetFullscreenRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowMode mode = Platform::Windowing::WindowMode::Windowed;
    };

    struct SetDecoratedRequest {
        Platform::Windowing::WindowId window{};
        bool decorated = true;
    };

    // See Window::set_transparent()'s own doc comment (Platform/Window/Window.hpp) for the real
    // per-OS constraints this ends up subject to — most notably, this no-ops with a warning on
    // Linux, where transparency can only be set at window-creation time (WindowConfig::transparent).
    struct SetTransparentRequest {
        Platform::Windowing::WindowId window{};
        bool transparent = false;
    };

    // `kind` names a specific native effect (Blur, Acrylic, Mica, ...) rather than always the
    // Window::set_blur_enabled() default — callers should pick from among
    // Platform::Windowing::operating_system_may_support_window_effect()-true kinds (see its own doc
    // comment) so this isn't sent for something the current OS silently no-ops+warns on.
    struct SetBlurRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowEffectKind kind = Platform::Windowing::WindowEffectKind::Blur;
        bool enabled = false;
    };

    // See Window::set_text_input_area()'s own doc comment — positions an IME composition candidate
    // window at a focused text field's caret. Fire-and-forget for the same reason as
    // SetCursorIconRequest above: a UI re-sends this every frame a focused field's bounds might have
    // changed, with nothing for a caller to await.
    struct SetTextInputAreaRequest {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::TextInputArea area{};
    };

    // Toggles start_text_input()/stop_text_input() — see their own doc comments (Window.hpp).
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

    // Deferred bridge from GameLogic/ECS/editor systems (which can reach Engine but deliberately do
    // not own live Platform windows) to Application (the process host that does). Requests are
    // drained once per main-loop tick after Engine::update(); completion records can be consumed on
    // a later update without exposing Application or WindowManager pointers below the host layer.
    class WindowRequests {
      public:
        [[nodiscard]] WindowRequestId spawn(const Platform::Windowing::WindowConfig &config,
                                            Platform::Windowing::WindowFactory factory = nullptr) {
            auto guard = state_.lock();
            const WindowRequestId id{guard->next_id++};
            guard->pending.emplace_back(SpawnWindowRequest{id, OwnedWindowConfig{config}, factory});
            return id;
        }

        [[nodiscard]] WindowRequestId close(Platform::Windowing::WindowId window) {
            auto guard = state_.lock();
            const WindowRequestId id{guard->next_id++};
            guard->pending.emplace_back(CloseWindowRequest{id, window});
            return id;
        }

        [[nodiscard]] WindowRequestId recreate_primary_window(const Platform::Windowing::WindowConfig &config,
                                                               Platform::Windowing::WindowFactory factory = nullptr) {
            auto guard = state_.lock();
            const WindowRequestId id{guard->next_id++};
            guard->pending.emplace_back(RecreatePrimaryWindowRequest{id, OwnedWindowConfig{config}, factory});
            return id;
        }

        // See SetCursorIconRequest's own doc comment for why this has no id/completion.
        void set_cursor_icon(Platform::Windowing::WindowId window, Platform::Windowing::CursorIcon icon) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetCursorIconRequest{window, icon});
        }

        // Borderless-fullscreen on via WindowMode::BorderlessFullscreen, off via WindowMode::Windowed
        // (ExclusiveFullscreen is available too — same underlying enum as Window::set_fullscreen()).
        void set_fullscreen(Platform::Windowing::WindowId window, Platform::Windowing::WindowMode mode) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetFullscreenRequest{window, mode});
        }

        // decorated=false removes the OS title bar/border, e.g. so the app can draw its own.
        void set_decorated(Platform::Windowing::WindowId window, bool decorated) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetDecoratedRequest{window, decorated});
        }

        void set_transparent(Platform::Windowing::WindowId window, bool transparent) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetTransparentRequest{window, transparent});
        }

        void set_blur(Platform::Windowing::WindowId window, Platform::Windowing::WindowEffectKind kind, bool enabled) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetBlurRequest{window, kind, enabled});
        }

        // See SetTextInputAreaRequest's own doc comment for why this has no id/completion. Typically
        // driven straight from a focused text_input()/text_area() result's own caret_bounds
        // (UI/TextInput.hpp, UI/TextArea.hpp) every frame it has one.
        void set_text_input_area(Platform::Windowing::WindowId window, Platform::Windowing::TextInputArea area) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetTextInputAreaRequest{window, area});
        }

        void set_text_input_active(Platform::Windowing::WindowId window, bool active) {
            auto guard = state_.lock();
            guard->pending.emplace_back(SetTextInputActiveRequest{window, active});
        }

        [[nodiscard]] vector<WindowRequest> drain() {
            auto guard = state_.lock();
            vector<WindowRequest> result;
            result.swap(guard->pending);
            return result;
        }

        void complete(WindowRequestCompletion completion) {
            auto guard = state_.lock();
            guard->completions.push_back(std::move(completion));
        }

        [[nodiscard]] vector<WindowRequestCompletion> take_completions() {
            auto guard = state_.lock();
            vector<WindowRequestCompletion> result;
            result.swap(guard->completions);
            return result;
        }

        [[nodiscard]] bool has_pending() const {
            auto guard = state_.lock();
            return !guard->pending.empty();
        }

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
