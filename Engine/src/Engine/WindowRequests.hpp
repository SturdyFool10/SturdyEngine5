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

    using WindowRequest = std::variant<SpawnWindowRequest, CloseWindowRequest, SetCursorIconRequest>;

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
