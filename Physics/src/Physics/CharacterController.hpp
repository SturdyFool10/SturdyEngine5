#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec3.hpp>

namespace SFT::Physics {

    struct SweepHit {
        /// Fraction of the requested movement completed before contact, in [0, 1].
        f32 fraction = 1.0f;
        /// Surface normal at the contact, facing the moving shape.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        glm::vec3 point{0.0f};
        /// Whatever was hit (body, entity), reported back so platforms can carry the character.
        u64 object = 0;
        /// Velocity of that object at the contact (zero for static geometry): moving platforms.
        glm::vec3 object_velocity{0.0f};
    };

    /// What the controller needs from a collision world. A solver adapter implements it with real capsule casts and
    /// overlap tests (filtering by layer inside the adapter); the controller itself is solver-agnostic.
    class CollisionQuery {
      public:
        virtual ~CollisionQuery() = default;

        /// Sweeps a vertical capsule whose feet are at `feet` (radius `radius`, total height `height` including the
        /// caps) by `delta`. Returns false when nothing is hit. A shape that starts inside geometry reports `fraction`
        /// 0 with the best escape normal.
        [[nodiscard]] virtual bool sweep_capsule(const glm::vec3 &feet, f32 radius, f32 height, const glm::vec3 &delta,
                                                 SweepHit &hit) const = 0;

        /// True when the capsule overlaps solid geometry.
        [[nodiscard]] virtual bool overlaps_capsule(const glm::vec3 &feet, f32 radius, f32 height) const = 0;
    };

    struct CharacterSettings {
        f32 radius = 0.3f;
        f32 standing_height = 1.8f;
        f32 crouching_height = 1.0f;
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        /// Gravity magnitude along `-up`.
        f32 gravity = 20.0f;
        /// Steepest walkable slope, degrees from horizontal.
        f32 slope_limit_degrees = 50.0f;
        /// Tallest ledge walked up without jumping.
        f32 step_height = 0.35f;
        /// How far to follow the ground downward so walking down ramps and stairs keeps contact.
        f32 snap_distance = 0.25f;
        /// Gap kept between the capsule and surfaces.
        f32 skin_width = 0.01f;
        u32 max_slides = 4;
        f32 jump_speed = 7.0f;
        /// Speed beyond which falling is clamped.
        f32 terminal_speed = 60.0f;
    };

    struct MoveResult {
        /// Position change this call (feet), including platform carry.
        glm::vec3 displacement{0.0f};
        bool grounded = false;
        bool hit_ceiling = false;
        bool stepped_up = false;
        bool jumped = false;
        /// Fell onto the ground this call; `landing_speed` is the speed along `-up` just before contact.
        bool landed = false;
        f32 landing_speed = 0.0f;
    };

    /// Kinematic character mover (collide-and-slide) in the manner of Jolt's CharacterVirtual: slides along walls and
    /// creases, refuses slopes over the limit, steps up ledges, snaps to the ground on ramps and stairs, rides moving
    /// platforms, and checks headroom before standing up from a crouch. It owns position and vertical velocity;
    /// horizontal velocity is whatever gameplay asks for each call.
    class CharacterController {
      public:
        explicit CharacterController(const CharacterSettings &settings = {}, const glm::vec3 &feet = glm::vec3(0.0f));

        /// Moves for `dt` seconds toward `desired_velocity` (horizontal, world space). `jump` launches when grounded.
        MoveResult move(const CollisionQuery &world, const glm::vec3 &desired_velocity, f32 dt, bool jump = false);

        /// Crouches immediately; standing up succeeds only with headroom (returns the resulting state).
        bool set_crouching(const CollisionQuery &world, bool crouching);

        void teleport(const glm::vec3 &feet) noexcept;
        void add_impulse(const glm::vec3 &velocity_change) noexcept { vertical_velocity_ += velocity_change; }

        [[nodiscard]] const glm::vec3 &position() const noexcept { return position_; }
        [[nodiscard]] glm::vec3 velocity() const noexcept { return vertical_velocity_; }
        [[nodiscard]] bool grounded() const noexcept { return grounded_; }
        [[nodiscard]] bool crouching() const noexcept { return crouching_; }
        [[nodiscard]] f32 height() const noexcept { return crouching_ ? settings_.crouching_height : settings_.standing_height; }
        [[nodiscard]] const glm::vec3 &ground_normal() const noexcept { return ground_normal_; }
        [[nodiscard]] u64 ground_object() const noexcept { return ground_object_; }
        [[nodiscard]] const CharacterSettings &settings() const noexcept { return settings_; }

      private:
        [[nodiscard]] bool walkable(const glm::vec3 &normal) const noexcept;
        /// Slides `delta` from `from`, returning the end point; reports whether anything blocked it.
        glm::vec3 slide(const CollisionQuery &world, glm::vec3 from, glm::vec3 delta, bool &blocked) const;
        /// Looks for ground within `distance` below `feet`.
        [[nodiscard]] bool probe_ground(const CollisionQuery &world, const glm::vec3 &feet, f32 distance, SweepHit &hit) const;

        CharacterSettings settings_;
        glm::vec3 position_{0.0f};
        // Velocity along `up` only (gravity, jumps, impulses); stored as a vector so impulses can be sideways-neutral.
        glm::vec3 vertical_velocity_{0.0f};
        bool grounded_ = false;
        bool crouching_ = false;
        glm::vec3 ground_normal_{0.0f, 1.0f, 0.0f};
        u64 ground_object_ = 0;
        glm::vec3 ground_velocity_{0.0f};
    };

} // namespace SFT::Physics
