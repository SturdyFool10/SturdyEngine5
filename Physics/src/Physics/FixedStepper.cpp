#include <Physics/FixedStepper.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Physics {

    FixedStepper::FixedStepper(f64 step_seconds, u32 max_steps_per_frame) noexcept
        : step_(step_seconds > 0.0 ? step_seconds : 1.0 / 60.0), max_steps_(std::max<u32>(max_steps_per_frame, 1)) {}

    u32 FixedStepper::advance(f64 delta_seconds) noexcept {
        const f64 delta = std::max(delta_seconds, 0.0);
        const f64 budget = step_ * static_cast<f64>(max_steps_);
        accumulator_ += delta;
        if (accumulator_ > budget) {
            dropped_ += accumulator_ - budget;
            accumulator_ = budget;
        }
        // The epsilon keeps 3 * (1/3) style sums from losing a step to rounding.
        const u32 steps = static_cast<u32>(std::floor(accumulator_ / step_ + 1e-9));
        accumulator_ = std::max(0.0, accumulator_ - static_cast<f64>(steps) * step_);
        alpha_ = std::clamp(accumulator_ / step_, 0.0, 1.0 - 1e-12);
        total_steps_ += steps;
        return steps;
    }

    void FixedStepper::set_step(f64 step_seconds) noexcept {
        if (step_seconds > 0.0) {
            step_ = step_seconds;
        }
    }

    void FixedStepper::reset() noexcept {
        accumulator_ = 0.0;
        alpha_ = 0.0;
        dropped_ = 0.0;
        total_steps_ = 0;
    }

    BodyPose interpolate(const BodyPose &previous, const BodyPose &current, f32 alpha) noexcept {
        BodyPose out;
        out.position = glm::mix(previous.position, current.position, alpha);
        glm::quat target = current.rotation;
        if (glm::dot(previous.rotation, target) < 0.0f) {
            target = -target;
        }
        out.rotation = glm::normalize(glm::slerp(previous.rotation, target, alpha));
        return out;
    }

} // namespace SFT::Physics
