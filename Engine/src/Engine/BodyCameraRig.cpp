#include <Engine/BodyCameraRig.hpp>

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace SFT::Engine {

    namespace {

        constexpr f64 kMaxFrame = 0.25; // a hitch longer than this is treated as a cut rather than simulated

        /// Advances a damped spring `x'' = -w^2 x - 2 zeta w x'` (x is the displacement from the target, held constant for the
        /// step) by exactly `t` seconds, in closed form: stable and exact at any frame time, unlike stepping an integrator
        /// (semi-implicit Euler at 240 Hz is already 35% off on a 9 Hz spring). Works on every component at once.
        void advance_spring(glm::vec3 &x, glm::vec3 &v, f32 omega, f32 zeta, f32 t) noexcept {
            const glm::vec3 x0 = x;
            const glm::vec3 v0 = v;
            if (std::abs(zeta - 1.0f) < 1.0e-4f) { // critically damped
                const f32 decay = std::exp(-omega * t);
                const glm::vec3 b = v0 + omega * x0;
                x = (x0 + b * t) * decay;
                v = (b - omega * (x0 + b * t)) * decay;
            } else if (zeta < 1.0f) { // underdamped: rings at the damped frequency
                const f32 damped = omega * std::sqrt(1.0f - zeta * zeta);
                const f32 decay = std::exp(-zeta * omega * t);
                const f32 c = std::cos(damped * t);
                const f32 s = std::sin(damped * t);
                const glm::vec3 k = (v0 + zeta * omega * x0) / damped;
                x = decay * (x0 * c + k * s);
                v = decay * (v0 * c - ((zeta * omega * v0 + omega * omega * x0) / damped) * s);
            } else { // overdamped: two real decaying modes
                const f32 root = omega * std::sqrt(zeta * zeta - 1.0f);
                const f32 r1 = -zeta * omega + root;
                const f32 r2 = -zeta * omega - root;
                const glm::vec3 c1 = (v0 - r2 * x0) / (r1 - r2);
                const glm::vec3 c2 = x0 - c1;
                const f32 e1 = std::exp(r1 * t);
                const f32 e2 = std::exp(r2 * t);
                x = c1 * e1 + c2 * e2;
                v = c1 * r1 * e1 + c2 * r2 * e2;
            }
        }

        [[nodiscard]] bool finite(const glm::vec3 &v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        [[nodiscard]] bool finite(const glm::quat &q) noexcept {
            return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
                   (q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z) > 1.0e-12f;
        }

        [[nodiscard]] u32 hash(u32 value) noexcept {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return value;
        }

        /// Smooth value noise in [-1, 1]: hashed lattice values, cubic-eased between them. `channel` picks an independent stream.
        [[nodiscard]] f32 smooth_noise(f64 t, u32 seed, u32 channel) noexcept {
            const f64 cell = std::floor(t);
            const f32 fraction = static_cast<f32>(t - cell);
            const u32 base = hash(seed * 0x9e3779b9u + channel * 0x85ebca6bu);
            const auto lattice = [&](i64 i) {
                return static_cast<f32>(hash(base + static_cast<u32>(i)) & 0xffffffu) / 8388607.5f - 1.0f; // [-1, 1]
            };
            const f32 eased = fraction * fraction * (3.0f - 2.0f * fraction);
            return lattice(static_cast<i64>(cell)) * (1.0f - eased) + lattice(static_cast<i64>(cell) + 1) * eased;
        }

        /// Shortest-path rotation vector (axis * angle, radians) that turns `from` into `to`.
        [[nodiscard]] glm::vec3 rotation_error(const glm::quat &from, const glm::quat &to) noexcept {
            glm::quat delta = to * glm::inverse(from);
            if (delta.w < 0.0f) {
                delta = -delta;
            }
            const glm::vec3 axis_part{delta.x, delta.y, delta.z};
            const f32 sine = glm::length(axis_part);
            if (sine < 1.0e-7f) {
                return axis_part * 2.0f;
            }
            return axis_part / sine * (2.0f * std::atan2(sine, delta.w));
        }

        [[nodiscard]] glm::quat rotation_from_vector(const glm::vec3 &v) noexcept {
            const f32 angle = glm::length(v);
            return angle < 1.0e-9f ? glm::quat{1.0f, 0.0f, 0.0f, 0.0f} : glm::angleAxis(angle, v / angle);
        }

    } // namespace

    void BodyCameraRig::reset(const glm::vec3 &position, const glm::quat &orientation) noexcept {
        position_ = finite(position) ? position : glm::vec3{0.0f};
        orientation_ = finite(orientation) ? glm::normalize(orientation) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        target_orientation_ = orientation_;
        velocity_ = glm::vec3{0.0f};
        angular_velocity_ = glm::vec3{0.0f};
        gait_amount_ = 0.0f;
    }

    void BodyCameraRig::update(f64 dt, const glm::vec3 &target_position, const glm::quat &target_orientation, f32 speed) noexcept {
        if (!(dt > 0.0) || !std::isfinite(dt) || !finite(target_position) || !finite(target_orientation)) {
            return;
        }
        if (dt > kMaxFrame) {
            // A long hitch (loading, a breakpoint): snap rather than integrate a huge, meaningless step.
            reset(target_position, target_orientation);
            time_ += dt;
            return;
        }
        const glm::quat target = glm::normalize(target_orientation);
        target_orientation_ = target;

        // Springs, solved exactly for this step with the aim held constant across it.
        const f32 zeta = std::clamp(settings_.damping_ratio, 0.05f, 4.0f);
        const f32 position_omega = glm::two_pi<f32>() * std::max(settings_.position_follow_hz, 0.01f);
        const f32 rotation_omega = glm::two_pi<f32>() * std::max(settings_.rotation_follow_hz, 0.01f);
        const f32 step = static_cast<f32>(dt);

        glm::vec3 displacement = position_ - target_position;
        advance_spring(displacement, velocity_, position_omega, zeta, step);
        position_ = target_position + displacement;

        // Orientation: the spring runs on the rotation vector from the aim to the camera; the change over the step is applied
        // as one rotation, which is exact for a fixed axis and a close approximation otherwise.
        const glm::vec3 before = -rotation_error(orientation_, target);
        glm::vec3 offset = before;
        advance_spring(offset, angular_velocity_, rotation_omega, zeta, step);
        orientation_ = glm::normalize(rotation_from_vector(offset - before) * orientation_);

        // Gait: fade the amount towards what the speed asks for, and advance the stride phase at speed / stride length.
        const f32 safe_speed = std::isfinite(speed) ? std::max(speed, 0.0f) : 0.0f;
        const f32 wanted = std::clamp(safe_speed / std::max(settings_.gait_reference_speed, 0.01f), 0.0f, 1.5f);
        const f32 blend = 1.0f - std::exp(-std::max(settings_.gait_blend_rate, 0.0f) * static_cast<f32>(dt));
        gait_amount_ += (wanted - gait_amount_) * blend;
        gait_phase_ += dt * static_cast<f64>(safe_speed / std::max(settings_.stride_length, 0.05f));
        gait_phase_ -= std::floor(gait_phase_);
        time_ += dt;
    }

    void BodyCameraRig::add_translation_impulse(const glm::vec3 &velocity) noexcept {
        if (finite(velocity)) velocity_ += velocity;
    }

    void BodyCameraRig::add_rotation_impulse(const glm::vec3 &angular_velocity) noexcept {
        if (finite(angular_velocity)) angular_velocity_ += angular_velocity;
    }

    glm::quat BodyCameraRig::orientation() const noexcept {
        const f32 radians = glm::radians(1.0f);
        const f64 drift = time_ * static_cast<f64>(settings_.handheld_frequency_hz);
        // Handheld wander about the camera's own axes, then the gait's roll and pitch on top.
        glm::vec3 local{smooth_noise(drift, settings_.seed, 0) * settings_.handheld_rotation_degrees * radians,
                        smooth_noise(drift, settings_.seed, 1) * settings_.handheld_rotation_degrees * radians,
                        smooth_noise(drift, settings_.seed, 2) * settings_.handheld_rotation_degrees * radians};
        const f32 stride = static_cast<f32>(gait_phase_ * 2.0 * glm::pi<f64>());
        local.z += std::sin(stride) * settings_.roll_amplitude_degrees * radians * gait_amount_;
        local.x += std::sin(2.0f * stride) * settings_.pitch_amplitude_degrees * radians * gait_amount_;
        return glm::normalize(orientation_ * rotation_from_vector(local));
    }

    glm::vec3 BodyCameraRig::position() const noexcept {
        const f64 drift = time_ * static_cast<f64>(settings_.handheld_frequency_hz);
        const glm::vec3 wander{smooth_noise(drift, settings_.seed, 3), smooth_noise(drift, settings_.seed, 4), smooth_noise(drift, settings_.seed, 5)};
        const f32 stride = static_cast<f32>(gait_phase_ * 2.0 * glm::pi<f64>());
        // Bob lifts the camera twice per stride (never below its rest height); sway swings it once per stride.
        const f32 bob = (0.5f - 0.5f * std::cos(2.0f * stride)) * settings_.bob_amplitude * gait_amount_;
        const f32 sway = std::sin(stride) * settings_.sway_amplitude * gait_amount_;
        const glm::vec3 right = orientation_ * glm::vec3{1.0f, 0.0f, 0.0f};
        return position_ + wander * settings_.handheld_position + glm::vec3{0.0f, bob, 0.0f} + right * sway;
    }

    glm::mat4 BodyCameraRig::pose_matrix() const noexcept {
        glm::mat4 pose = glm::mat4_cast(orientation());
        pose[3] = glm::vec4{position(), 1.0f};
        return pose;
    }

} // namespace SFT::Engine
