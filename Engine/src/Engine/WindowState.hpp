#pragma once

#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace SFT::Engine {

    // Read-only per-window snapshot of state a gameplay/UI system might want without touching a live
    // Window*. Window is main-thread-affine (Window.hpp: "do not operate one window from multiple
    // threads"), and ECS Schedule stages run on Async::Scheduler worker threads, so Application builds
    // one of these per managed window on the window-owning thread and hands it to Engine as plain
    // data every tick — exactly how PreparedRenderFrame keeps ECS workers away from live
    // Renderer/RHI objects.
    struct WindowSnapshot {
        Platform::Windowing::WindowId id{};
        Platform::Windowing::WindowExtent size{};
        Platform::Windowing::WindowExtent framebuffer_size{};
        Platform::Windowing::WindowPosition position{};
        f32 opacity = 1.0f;
        bool mouse_locked = false;
        // Window has no direct getter for this, only the one-shot WindowEventKind::FocusGained/
        // FocusLost events, so Application latches it across ticks. There is deliberately no
        // minimized/maximized field: Platform doesn't surface either as a queryable state or an event
        // today (Window::minimize()/maximize() are write-only requests) — add one only once Platform
        // actually reports it, rather than guessing at a proxy signal.
        bool focused = false;
    };

    // Ordinary World resource: the read side of "what do the managed windows currently look like."
    // Engine owns the persistent instance and Application calls sync() once per tick with a fresh
    // snapshot of every managed window — mutated in place, never rebound, so a
    // Ecs::ReadResource<WindowState> a system holds mid-tick is never invalidated by the update
    // itself (the same contract RenderFrameRequests already relies on).
    class WindowState {
      public:
        void sync(std::vector<WindowSnapshot> windows, std::optional<Platform::Windowing::WindowId> primary) noexcept {
            windows_ = std::move(windows);
            primary_ = primary;
        }

        [[nodiscard]] std::span<const WindowSnapshot> windows() const noexcept { return windows_; }

        [[nodiscard]] const WindowSnapshot *find(Platform::Windowing::WindowId id) const noexcept {
            for (const WindowSnapshot &snapshot : windows_) {
                if (snapshot.id == id) {
                    return &snapshot;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const WindowSnapshot *primary() const noexcept {
            return primary_ ? find(*primary_) : nullptr;
        }

      private:
        std::vector<WindowSnapshot> windows_;
        std::optional<Platform::Windowing::WindowId> primary_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowState, "sturdy.engine.window_state");
