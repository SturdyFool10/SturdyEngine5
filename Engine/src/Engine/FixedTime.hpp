#pragma once

#include <Ecs/Resource.hpp>
#include <Foundation/Foundation.hpp>

#include <Physics/FixedStepper.hpp>

namespace SFT::Engine {

    /// The fixed-timestep clock for simulation systems (physics, cloth, deterministic gameplay). Each engine update
    /// advances it by the scaled frame delta; a system then runs its step `steps()` times with `step_seconds()` each
    /// and renders at `alpha()` between its last two states.
    class FixedTime {
      public:
        void advance(f64 scaled_delta_seconds) noexcept { steps_ = stepper_.advance(scaled_delta_seconds); }
        /// Fixed steps due during the current update.
        [[nodiscard]] u32 steps() const noexcept { return steps_; }
        [[nodiscard]] f64 step_seconds() const noexcept { return stepper_.step(); }
        /// Interpolation factor in [0, 1) between the last two simulated states.
        [[nodiscard]] f64 alpha() const noexcept { return stepper_.alpha(); }
        [[nodiscard]] u64 total_steps() const noexcept { return stepper_.total_steps(); }
        void set_step_seconds(f64 seconds) noexcept { stepper_.set_step(seconds); }
        /// Seconds dropped by the spiral-of-death clamp (a sign the simulation cannot keep up).
        [[nodiscard]] f64 dropped_seconds() const noexcept { return stepper_.dropped_seconds(); }

      private:
        Physics::FixedStepper stepper_{};
        u32 steps_ = 0;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::FixedTime, "sturdy.engine.fixed_time");
