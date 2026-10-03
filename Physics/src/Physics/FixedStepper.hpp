#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace SFT::Physics {

    /// Fixed-timestep accumulator ("Fix Your Timestep"): feed it the frame's variable delta, run the simulation
    /// `advance()` times at `step()` seconds each, then render at `alpha()` between the last two states.
    class FixedStepper {
      public:
        explicit FixedStepper(f64 step_seconds = 1.0 / 60.0, u32 max_steps_per_frame = 8) noexcept;

        /// Adds `delta_seconds` and returns how many fixed steps are now due. Time beyond `max_steps_per_frame`
        /// steps is dropped, so one long frame (a breakpoint, a hitch) cannot trigger a spiral of death.
        [[nodiscard]] u32 advance(f64 delta_seconds) noexcept;

        /// Fraction of a step the accumulator holds after `advance()`, in [0, 1): the interpolation factor.
        [[nodiscard]] f64 alpha() const noexcept { return alpha_; }
        [[nodiscard]] f64 step() const noexcept { return step_; }
        [[nodiscard]] u64 total_steps() const noexcept { return total_steps_; }
        /// Simulated seconds so far (`total_steps() * step()`).
        [[nodiscard]] f64 simulated_time() const noexcept { return static_cast<f64>(total_steps_) * step_; }
        /// Seconds discarded by the spiral-of-death clamp since the last `reset`.
        [[nodiscard]] f64 dropped_seconds() const noexcept { return dropped_; }

        void set_step(f64 step_seconds) noexcept;
        void reset() noexcept;

      private:
        f64 step_ = 1.0 / 60.0;
        u32 max_steps_ = 8;
        f64 accumulator_ = 0.0;
        f64 alpha_ = 0.0;
        f64 dropped_ = 0.0;
        u64 total_steps_ = 0;
    };

    /// Position + orientation of a body at one fixed step, for render interpolation.
    struct BodyPose {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// The pose to draw: `previous` and `current` are the last two simulation states, `alpha` comes from
    /// `FixedStepper::alpha()`. Rotation takes the short way round.
    [[nodiscard]] BodyPose interpolate(const BodyPose &previous, const BodyPose &current, f32 alpha) noexcept;

} // namespace SFT::Physics
