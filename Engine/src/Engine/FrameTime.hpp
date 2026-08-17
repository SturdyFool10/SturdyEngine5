#pragma once

#include <Ecs/src/Resource.hpp>
#include <Foundation/src/Foundation.hpp>

namespace SFT::Engine {

    /// Global, window-independent frame timing for Ecs::ReadResource<FrameTime> consumers. Engine owns
    /// the persistent instance; Engine::update(delta_seconds) advances it once per call, before running
    /// update_schedule_, so every system in that schedule's stages sees the same delta for the whole
    /// tick regardless of how many (or how few) windows are attached — unlike Core::FrameInput, which
    /// is deliberately per-window (see WindowState.hpp's own doc comment on why that split exists).
    ///
    /// delta_seconds() is scaled by TimeScale (see TimeScale.hpp) — the delta most gameplay systems
    /// want, since it's what pauses/slows/speeds up under a game's own time controls.
    /// unscaled_delta_seconds() is real wall-clock time, for UI/editor/profiling systems that must keep
    /// moving at normal speed even while gameplay is paused or in slow motion.
    class FrameTime {
      public:
        void advance(f64 unscaled_delta_seconds, f64 scale) noexcept;

        [[nodiscard]] f64 delta_seconds() const noexcept;
        [[nodiscard]] f64 unscaled_delta_seconds() const noexcept;
        [[nodiscard]] u64 tick_index() const noexcept;

      private:
        f64 delta_seconds_ = 0.0;
        f64 unscaled_delta_seconds_ = 0.0;
        u64 tick_index_ = 0;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::FrameTime, "sturdy.engine.frame_time");
