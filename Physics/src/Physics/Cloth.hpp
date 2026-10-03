#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace SFT::Physics {

    struct ClothSphere {
        glm::vec3 center{0.0f};
        f32 radius = 0.1f;
    };

    struct ClothCapsule {
        glm::vec3 a{0.0f};
        glm::vec3 b{0.0f, 1.0f, 0.0f};
        f32 radius = 0.1f;
    };

    /// Plane through `normal * offset` facing `normal`; particles are kept on the positive side.
    struct ClothPlane {
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        f32 offset = 0.0f;
    };

    /// Collision proxies for one step, in the cloth's space. Gameplay one-way-couples cloth to the rigid world by
    /// filling these from the solver's body shapes (a vest around a torso capsule, a flag against a pole).
    struct ClothColliders {
        std::span<const ClothSphere> spheres;
        std::span<const ClothCapsule> capsules;
        std::span<const ClothPlane> planes;
    };

    struct ClothParams {
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        /// XPBD compliance (inverse stiffness, m/N): 0 is rigid; stretch should stay near zero, bending is softer.
        f32 stretch_compliance = 0.0f;
        f32 bend_compliance = 5e-3f;
        /// Fraction of velocity lost per second.
        f32 damping = 0.2f;
        /// Constraint iterations per step; each substep is a full predict/solve/update, so more substeps trade cost
        /// for stiffness and stability.
        u32 substeps = 8;
        /// Radius added around colliders so the surface does not sit inside them.
        f32 collision_thickness = 0.005f;
        /// Fraction of tangential motion removed on contact.
        f32 friction = 0.4f;
        /// Uniform wind velocity and how strongly the cloth catches it.
        glm::vec3 wind{0.0f};
        f32 drag = 0.5f;
    };

    /// Position-based dynamics cloth (XPBD, after Macklin/Müller): stretch constraints on every edge, bending as
    /// distance constraints between the far vertices of adjacent triangles, substepped Gauss-Seidel, collision against
    /// spheres, capsules and planes, optional wind. Particles are pinned by giving them zero inverse mass.
    class Cloth {
      public:
        /// A `columns x rows` sheet of particles `spacing` apart, starting at `origin` and extending along `right` and
        /// `down` (unit vectors). Mass is spread evenly over `total_mass`.
        [[nodiscard]] static Cloth grid(u32 columns, u32 rows, f32 spacing, const glm::vec3 &origin, const glm::vec3 &right,
                                        const glm::vec3 &down, f32 total_mass = 1.0f);

        /// Any triangle mesh; the rest lengths are the edge lengths in `positions`.
        [[nodiscard]] static Cloth from_triangles(std::span<const glm::vec3> positions, std::span<const u32> indices,
                                                  f32 total_mass = 1.0f);

        [[nodiscard]] usize particle_count() const noexcept { return positions_.size(); }
        [[nodiscard]] std::span<const glm::vec3> positions() const noexcept { return positions_; }
        [[nodiscard]] std::span<const u32> indices() const noexcept { return indices_; }
        [[nodiscard]] usize stretch_constraint_count() const noexcept { return stretch_.size(); }
        [[nodiscard]] usize bend_constraint_count() const noexcept { return bend_.size(); }

        /// Pins (or releases) a particle in place. Pinned particles can be moved with `move_pinned`.
        void pin(u32 particle, bool pinned) noexcept;
        [[nodiscard]] bool is_pinned(u32 particle) const noexcept;
        /// Moves a pinned particle (attachment points following an animated body).
        void move_pinned(u32 particle, const glm::vec3 &position) noexcept;

        /// Advances by `dt` seconds.
        void step(f32 dt, const ClothParams &params, const ClothColliders &colliders = {});

        /// Smooth per-particle normals from the current positions (resized to `particle_count()`).
        void compute_normals(std::vector<glm::vec3> &out) const;

      private:
        struct Distance {
            u32 a = 0;
            u32 b = 0;
            f32 rest = 0.0f;
        };

        void build_constraints();
        void apply_wind(f32 dt, const ClothParams &params);
        void collide(const ClothParams &params, const ClothColliders &colliders);

        std::vector<glm::vec3> positions_;
        std::vector<glm::vec3> previous_;
        std::vector<glm::vec3> velocities_;
        std::vector<f32> inverse_mass_;
        std::vector<f32> rest_inverse_mass_;
        std::vector<u32> indices_;
        std::vector<Distance> stretch_;
        std::vector<Distance> bend_;
        std::vector<f32> stretch_lambda_;
        std::vector<f32> bend_lambda_;
    };

} // namespace SFT::Physics
