#pragma once

#include <Foundation/Foundation.hpp>

#include <optional>
#include <vector>

#include <Ecs/System.hpp>
#include <Engine/EcsEvents.hpp>
#include <Renderer/UI/UI.hpp>
#include <WindowManager/WindowManager.hpp>

namespace SFT::Engine {

    class Engine;

    /// Translates window events into `UI::UiInput` calls. This is the whole of "window input drives a UI"; use it
    /// directly to feed a UI from events you route yourself (one UI per window, an in-world UI you only forward
    /// events to while it has focus).
    void apply_window_event(UI::UiInput &input, const MouseMoveEvent &event) noexcept;
    void apply_window_event(UI::UiInput &input, const MouseButtonEvent &event) noexcept;
    void apply_window_event(UI::UiInput &input, const MouseWheelEvent &event) noexcept;
    void apply_window_event(UI::UiInput &input, const KeyboardEvent &event);
    void apply_window_event(UI::UiInput &input, const TextInputEvent &event);
    void apply_window_event(UI::UiInput &input, const TextEditingEvent &event);

    /// The on-screen UI, the easy default: one `UI::UiSurface` fed by the window's mouse, keyboard and text events,
    /// drawn as an overlay on top of the frame.
    ///
    /// It is application code, not engine machinery: create one wherever your game keeps its state, and the engine
    /// neither owns nor knows about it. Construction adds an input-forwarding system to the update schedule; it is
    /// removed again on destruction. Not copyable or movable (the system refers to it).
    ///
    /// ```cpp
    /// // once
    /// Engine::ScreenUi hud{engine};
    /// // every frame
    /// if (hud.ensure_ready(color_format)) {
    ///     UI::Context &ctx = hud.begin_frame(viewport, dt);
    ///     ... widgets ...
    ///     params.overlay_passes.push_back(hud.finish_overlay());
    /// }
    /// // hud.input().pointer_consumed() says whether a click landed on the UI
    /// ```
    ///
    /// For an in-world UI, use `UI::UiSurface` directly instead and feed its `input()` from your own pointing
    /// device (see `UI/UiWorld.hpp`); `ScreenUi` is just a `UiSurface` plus window input.
    class ScreenUi {
      public:
        explicit ScreenUi(Engine &engine);
        ~ScreenUi();
        ScreenUi(const ScreenUi &) = delete;
        ScreenUi &operator=(const ScreenUi &) = delete;

        /// Only events from this window drive the UI. Default: every window (a single-window game).
        void set_window(std::optional<WindowManager::WindowId> window) noexcept { window_ = window; }

        [[nodiscard]] UI::UiSurface &surface() noexcept { return surface_; }
        [[nodiscard]] UI::UiInput &input() noexcept { return surface_.input(); }
        [[nodiscard]] UI::Context &context() noexcept { return surface_.context(); }

        /// See `UI::UiSurface::ensure_ready`; uses the engine's device.
        [[nodiscard]] bool ensure_ready(RHI::Format color_format);
        UI::Context &begin_frame(glm::vec2 viewport, f32 delta_seconds) { return surface_.begin_frame(viewport, delta_seconds); }
        /// The overlay to append to `RenderFrameParameters::overlay_passes`.
        [[nodiscard]] Renderer::OverlayPass finish_overlay(UI::UiOverlayOptions options = {});

        /// Releases the UI's GPU renderer now (a graphics reconstruction, an HDR toggle); the layout context and its
        /// fonts are kept. `ensure_ready` recreates the renderer.
        void release_gpu_objects() noexcept;

      private:
        Engine &engine_;
        UI::UiSurface surface_{};
        std::optional<WindowManager::WindowId> window_{};
        std::vector<Ecs::SystemHandle> systems_;
    };

} // namespace SFT::Engine
