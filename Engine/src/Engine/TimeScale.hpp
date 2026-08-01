#pragma once

#include <Ecs/src/Resource.hpp>
#include <Foundation/src/Foundation.hpp>

namespace SFT::Engine {

    // Global time dilation multiplier for gameplay/tooling to slow down, speed up, or pause
    // simulation time without touching real (wall-clock) delta. Ecs::WriteResource<TimeScale> lets
    // any system change it (a pause menu, a bullet-time ability, an editor timeline scrubber);
    // Engine::update() reads it once per call to scale the delta it feeds into FrameTime — see
    // FrameTime.hpp for the resulting scaled/unscaled split. Negative scales are rejected (clamped to
    // zero) since nothing in the engine is designed to run simulation time backwards.
    class TimeScale {
      public:
        void set(f64 scale) noexcept { scale_ = scale > 0.0 ? scale : 0.0; }

        [[nodiscard]] f64 value() const noexcept { return scale_; }

      private:
        f64 scale_ = 1.0;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::TimeScale, "sturdy.engine.time_scale");
