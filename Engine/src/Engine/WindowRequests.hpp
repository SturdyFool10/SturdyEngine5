#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Core/Core.hpp>
#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace SFT::Engine {

    struct WindowRequestId {
        u64 value = 0;
        [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
        [[nodiscard]] auto operator<=>(const WindowRequestId &) const = default;
    };

    // Owns WindowConfig's otherwise-borrowed title so requests can safely wait in a queue until the
    // Application host reaches its window-owner phase.
    struct OwnedWindowConfig {
        std::string title{"Sturdy Engine"};
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

    using WindowRequest = std::variant<SpawnWindowRequest, CloseWindowRequest, SetCursorIconRequest,
                                       SetFullscreenRequest, SetDecoratedRequest, SetTransparentRequest, SetBlurRequest>;

    enum class WindowRequestKind : u8 { Spawn, Close };

    struct WindowRequestCompletion {
        WindowRequestId id{};
        WindowRequestKind kind = WindowRequestKind::Spawn;
        bool accepted = false;
        std::optional<Core::RenderSurfaceHandle> surface;
        Platform::Windowing::WindowId window{};
        std::string message;
    };

    // Deferred bridge from GameLogic/ECS/editor systems (which can reach Engine but deliberately do
    // not own live Platform windows) to Application (the process host that does). Requests are
    // drained once per main-loop tick after Engine::update(); completion records can be consumed on
    // a later update without exposing Application or WindowManager pointers below the host layer.
    class WindowRequests {
      public:
        [[nodiscard]] WindowRequestId spawn(const Platform::Windowing::WindowConfig &config,
                                            Platform::Windowing::WindowFactory factory = nullptr) {
            const std::lock_guard lock{mutex_};
            const WindowRequestId id{next_id_++};
            pending_.emplace_back(SpawnWindowRequest{id, OwnedWindowConfig{config}, factory});
            return id;
        }

        [[nodiscard]] WindowRequestId close(Platform::Windowing::WindowId window) {
            const std::lock_guard lock{mutex_};
            const WindowRequestId id{next_id_++};
            pending_.emplace_back(CloseWindowRequest{id, window});
            return id;
        }

        // See SetCursorIconRequest's own doc comment for why this has no id/completion.
        void set_cursor_icon(Platform::Windowing::WindowId window, Platform::Windowing::CursorIcon icon) {
            const std::lock_guard lock{mutex_};
            pending_.emplace_back(SetCursorIconRequest{window, icon});
        }

        // Borderless-fullscreen on via WindowMode::BorderlessFullscreen, off via WindowMode::Windowed
        // (ExclusiveFullscreen is available too — same underlying enum as Window::set_fullscreen()).
        void set_fullscreen(Platform::Windowing::WindowId window, Platform::Windowing::WindowMode mode) {
            const std::lock_guard lock{mutex_};
            pending_.emplace_back(SetFullscreenRequest{window, mode});
        }

        // decorated=false removes the OS title bar/border, e.g. so the app can draw its own.
        void set_decorated(Platform::Windowing::WindowId window, bool decorated) {
            const std::lock_guard lock{mutex_};
            pending_.emplace_back(SetDecoratedRequest{window, decorated});
        }

        void set_transparent(Platform::Windowing::WindowId window, bool transparent) {
            const std::lock_guard lock{mutex_};
            pending_.emplace_back(SetTransparentRequest{window, transparent});
        }

        void set_blur(Platform::Windowing::WindowId window, Platform::Windowing::WindowEffectKind kind, bool enabled) {
            const std::lock_guard lock{mutex_};
            pending_.emplace_back(SetBlurRequest{window, kind, enabled});
        }

        [[nodiscard]] std::vector<WindowRequest> drain() {
            const std::lock_guard lock{mutex_};
            std::vector<WindowRequest> result;
            result.swap(pending_);
            return result;
        }

        void complete(WindowRequestCompletion completion) {
            const std::lock_guard lock{mutex_};
            completions_.push_back(std::move(completion));
        }

        [[nodiscard]] std::vector<WindowRequestCompletion> take_completions() {
            const std::lock_guard lock{mutex_};
            std::vector<WindowRequestCompletion> result;
            result.swap(completions_);
            return result;
        }

        [[nodiscard]] bool has_pending() const {
            const std::lock_guard lock{mutex_};
            return !pending_.empty();
        }

      private:
        mutable std::mutex mutex_;
        u64 next_id_ = 1;
        std::vector<WindowRequest> pending_;
        std::vector<WindowRequestCompletion> completions_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowRequests, "sturdy.engine.window_requests");
