#pragma once

#include <Physics/CharacterController.hpp>

#include <vector>

namespace SFT::Physics {

    /// A minimal analytic collision world (boxes and planes) implementing `CollisionQuery`, for tools, tests and
    /// quick prototypes without a physics solver. Capsules point along +Y and are approximated by spheres at their two
    /// ends and middle, which is exact for flat floors and walls and slightly generous at box corners.
    class SimpleWorld final : public CollisionQuery {
      public:
        struct Box {
            glm::vec3 min{0.0f};
            glm::vec3 max{1.0f};
            u64 object = 0;
            glm::vec3 velocity{0.0f}; // reported to the controller so it can ride the box
        };
        struct Plane {
            glm::vec3 normal{0.0f, 1.0f, 0.0f};
            f32 offset = 0.0f;
            u64 object = 0;
        };

        void add_box(const Box &box) { boxes_.push_back(box); }
        void add_plane(const Plane &plane) { planes_.push_back(plane); }
        /// Moves a box (a platform) by `delta`.
        void move_box(usize index, const glm::vec3 &delta) {
            if (index < boxes_.size()) {
                boxes_[index].min += delta;
                boxes_[index].max += delta;
            }
        }

        bool sweep_capsule(const glm::vec3 &feet, f32 radius, f32 height, const glm::vec3 &delta, SweepHit &hit) const override;
        bool overlaps_capsule(const glm::vec3 &feet, f32 radius, f32 height) const override;

      private:
        bool sweep_sphere(const glm::vec3 &center, f32 radius, const glm::vec3 &delta, SweepHit &hit) const;

        std::vector<Box> boxes_;
        std::vector<Plane> planes_;
    };

} // namespace SFT::Physics
