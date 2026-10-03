#include <Physics/CharacterController.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace SFT::Physics {

    CharacterController::CharacterController(const CharacterSettings &settings, const glm::vec3 &feet)
        : settings_(settings), position_(feet) {
        settings_.up = glm::normalize(settings_.up);
    }

    void CharacterController::teleport(const glm::vec3 &feet) noexcept {
        position_ = feet;
        vertical_velocity_ = glm::vec3(0.0f);
        grounded_ = false;
        ground_object_ = 0;
        ground_velocity_ = glm::vec3(0.0f);
    }

    bool CharacterController::walkable(const glm::vec3 &normal) const noexcept {
        return glm::dot(normal, settings_.up) >= std::cos(glm::radians(settings_.slope_limit_degrees));
    }

    bool CharacterController::probe_ground(const CollisionQuery &world, const glm::vec3 &feet, f32 distance, SweepHit &hit) const {
        if (distance <= 0.0f) {
            return false;
        }
        SweepHit candidate;
        if (!world.sweep_capsule(feet, settings_.radius, height(), -settings_.up * distance, candidate)) {
            return false;
        }
        if (!walkable(candidate.normal)) {
            return false;
        }
        hit = candidate;
        return true;
    }

    glm::vec3 CharacterController::slide(const CollisionQuery &world, glm::vec3 from, glm::vec3 delta, bool &blocked) const {
        blocked = false;
        std::array<glm::vec3, 3> planes{};
        u32 plane_count = 0;
        const f32 grounded_cos = std::cos(glm::radians(settings_.slope_limit_degrees));

        for (u32 iteration = 0; iteration < settings_.max_slides && glm::dot(delta, delta) > 1e-10f; ++iteration) {
            SweepHit hit;
            if (!world.sweep_capsule(from, settings_.radius, height(), delta, hit)) {
                from += delta;
                return from;
            }
            blocked = true;
            from += delta * std::max(hit.fraction, 0.0f);
            from += hit.normal * settings_.skin_width; // never rest exactly on a surface
            glm::vec3 remaining = delta * (1.0f - std::clamp(hit.fraction, 0.0f, 1.0f));

            // Clip against the plane just hit, and against earlier planes so a crease does not push us through.
            if (plane_count < planes.size()) {
                planes[plane_count++] = hit.normal;
            }
            remaining -= hit.normal * std::min(glm::dot(remaining, hit.normal), 0.0f);
            if (plane_count >= 2) {
                const glm::vec3 &a = planes[plane_count - 2];
                const glm::vec3 &b = planes[plane_count - 1];
                const glm::vec3 crease = glm::cross(a, b);
                const f32 crease_length = glm::length(crease);
                if (crease_length > 1e-4f && (glm::dot(remaining, a) < -1e-5f || glm::dot(remaining, b) < -1e-5f)) {
                    const glm::vec3 direction = crease / crease_length;
                    remaining = direction * glm::dot(remaining, direction);
                }
            }
            // A slope too steep to walk must not carry the character up it.
            if (glm::dot(hit.normal, settings_.up) < grounded_cos && glm::dot(hit.normal, settings_.up) > -0.7f) {
                const f32 upward = glm::dot(remaining, settings_.up);
                if (upward > 0.0f) {
                    remaining -= settings_.up * upward;
                }
            }
            delta = remaining;
        }
        return from;
    }

    MoveResult CharacterController::move(const CollisionQuery &world, const glm::vec3 &desired_velocity, f32 dt, bool jump) {
        MoveResult result;
        if (dt <= 0.0f) {
            result.grounded = grounded_;
            return result;
        }
        const glm::vec3 start = position_;
        const glm::vec3 up = settings_.up;
        const bool was_grounded = grounded_;
        const f32 falling_speed_before = -glm::dot(vertical_velocity_, up);

        // Ride whatever we stand on.
        if (was_grounded && glm::dot(ground_velocity_, ground_velocity_) > 1e-10f) {
            bool blocked = false;
            position_ = slide(world, position_, ground_velocity_ * dt, blocked);
        }

        // Vertical velocity: gravity, or a jump off the ground.
        if (was_grounded && jump) {
            vertical_velocity_ = up * settings_.jump_speed;
            grounded_ = false;
            result.jumped = true;
        } else if (was_grounded) {
            vertical_velocity_ = glm::vec3(0.0f);
        } else {
            vertical_velocity_ -= up * (settings_.gravity * dt);
            const f32 speed = -glm::dot(vertical_velocity_, up);
            if (speed > settings_.terminal_speed) {
                vertical_velocity_ = -up * settings_.terminal_speed;
            }
        }

        // Horizontal movement, with step-up when a ledge blocks it.
        glm::vec3 horizontal = desired_velocity - up * glm::dot(desired_velocity, up);
        const glm::vec3 horizontal_delta = horizontal * dt;
        if (glm::dot(horizontal_delta, horizontal_delta) > 1e-12f) {
            bool blocked = false;
            glm::vec3 slid = slide(world, position_, horizontal_delta, blocked);
            if (blocked && (was_grounded || grounded_) && settings_.step_height > 0.0f) {
                // Try again from one step higher, then come back down onto whatever is there.
                SweepHit up_hit;
                f32 rise = settings_.step_height;
                if (world.sweep_capsule(position_, settings_.radius, height(), up * rise, up_hit)) {
                    rise = std::max(0.0f, up_hit.fraction * rise - settings_.skin_width);
                }
                if (rise > settings_.skin_width) {
                    const glm::vec3 raised = position_ + up * rise;
                    bool step_blocked = false;
                    const glm::vec3 across = slide(world, raised, horizontal_delta, step_blocked);
                    SweepHit down_hit;
                    if (world.sweep_capsule(across, settings_.radius, height(), -up * (rise + settings_.skin_width), down_hit) &&
                        walkable(down_hit.normal)) {
                        const glm::vec3 landed = across - up * (down_hit.fraction * (rise + settings_.skin_width));
                        const f32 direct_progress = glm::dot(slid - position_, horizontal_delta);
                        const f32 step_progress = glm::dot(landed - position_, horizontal_delta);
                        if (step_progress > direct_progress + 1e-4f) {
                            slid = landed;
                            result.stepped_up = true;
                        }
                    }
                }
            }
            position_ = slid;
        }

        // Vertical movement.
        const glm::vec3 vertical_delta = vertical_velocity_ * dt;
        if (glm::dot(vertical_delta, vertical_delta) > 1e-12f) {
            SweepHit hit;
            if (world.sweep_capsule(position_, settings_.radius, height(), vertical_delta, hit)) {
                position_ += vertical_delta * std::max(hit.fraction, 0.0f) + hit.normal * settings_.skin_width;
                if (glm::dot(vertical_delta, up) > 0.0f) {
                    result.hit_ceiling = true;
                    vertical_velocity_ -= up * glm::dot(vertical_velocity_, up);
                } else if (walkable(hit.normal)) {
                    vertical_velocity_ = glm::vec3(0.0f);
                }
            } else {
                position_ += vertical_delta;
            }
        }

        // Ground contact: probe a little way down, further when we were grounded so slopes and stairs are followed.
        grounded_ = false;
        ground_object_ = 0;
        ground_velocity_ = glm::vec3(0.0f);
        if (glm::dot(vertical_velocity_, up) <= 0.0f) {
            const f32 reach = was_grounded && !result.jumped ? settings_.snap_distance : settings_.skin_width * 4.0f;
            SweepHit ground;
            if (probe_ground(world, position_, reach + settings_.skin_width, ground)) {
                position_ -= up * (ground.fraction * (reach + settings_.skin_width) - settings_.skin_width);
                grounded_ = true;
                ground_normal_ = ground.normal;
                ground_object_ = ground.object;
                ground_velocity_ = ground.object_velocity;
                vertical_velocity_ = glm::vec3(0.0f);
            }
        }

        if (grounded_ && !was_grounded) {
            result.landed = true;
            result.landing_speed = std::max(falling_speed_before, 0.0f);
        }
        result.grounded = grounded_;
        result.displacement = position_ - start;
        return result;
    }

    bool CharacterController::set_crouching(const CollisionQuery &world, bool crouching) {
        if (crouching == crouching_) {
            return crouching_;
        }
        if (crouching) {
            crouching_ = true;
            return true;
        }
        // Standing up needs the full-height capsule to be clear.
        if (!world.overlaps_capsule(position_ + settings_.up * settings_.skin_width, settings_.radius, settings_.standing_height - settings_.skin_width)) {
            crouching_ = false;
        }
        return crouching_;
    }

} // namespace SFT::Physics
