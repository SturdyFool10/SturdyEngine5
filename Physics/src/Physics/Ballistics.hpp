#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <functional>
#include <vector>

/// Projectile-versus-surface resolution: penetration, ricochet and energy loss from per-material properties, and a
/// multi-surface shot tracer that handles walls-behind-walls with exit points. Pure maths over a caller-supplied ray
/// query, so it works with any solver (and on a server rewinding lag-compensated state).
namespace SFT::Physics {

    using MaterialId = u32;
    inline constexpr MaterialId no_material = 0xFFFFFFFFu;

    struct SurfaceMaterial {
        UString name;
        /// Energy a projectile spends per metre travelled through the material, in joules per metre. Drywall ~ 400,
        /// timber ~ 2500, concrete ~ 25000, steel plate ~ 90000 for a rifle round.
        f32 penetration_resistance = 5000.0f;
        /// Impact angles measured from the surface (0 = skimming, 90 = square on) below which the projectile skips
        /// off instead of entering.
        f32 ricochet_angle_degrees = 10.0f;
        /// Fraction of energy kept by a ricochet.
        f32 ricochet_energy_retained = 0.5f;
        /// How far a ricochet direction scatters, in radians of cone half-angle at the grazing limit.
        f32 ricochet_scatter = 0.08f;
        /// How far a projectile bends coming out the other side, in radians per unit of (depth / max depth).
        f32 exit_deflection = 0.12f;
    };

    /// Indexed material table, shared by gameplay, audio and VFX (impact sounds and decals key off `MaterialId` too).
    class SurfaceTable {
      public:
        MaterialId add(SurfaceMaterial material);
        [[nodiscard]] MaterialId find(const ustr &name) const noexcept;
        [[nodiscard]] const SurfaceMaterial *get(MaterialId id) const noexcept;
        [[nodiscard]] usize size() const noexcept { return materials_.size(); }

      private:
        std::vector<SurfaceMaterial> materials_;
    };

    struct Projectile {
        glm::vec3 position{0.0f};
        /// Unit travel direction.
        glm::vec3 direction{0.0f, 0.0f, -1.0f};
        /// Kinetic energy in joules.
        f32 energy = 3000.0f;
    };

    enum class ImpactOutcome : u8 {
        Stopped,    // absorbed inside the material
        Penetrated, // came out the far side
        Ricocheted, // skipped off the entry surface
    };

    struct ImpactResult {
        ImpactOutcome outcome = ImpactOutcome::Stopped;
        /// Where the projectile stopped (Stopped), left the material (Penetrated) or bounced (Ricocheted).
        glm::vec3 point{0.0f};
        /// Travel direction afterwards (meaningless for Stopped).
        glm::vec3 direction{0.0f};
        f32 remaining_energy = 0.0f;
        /// Distance travelled inside the material.
        f32 depth = 0.0f;
    };

    /// Resolves one entry into one surface. `normal` faces the projectile, `thickness` is the distance the straight
    /// ray spends inside before the exit point (infinite or very large for terrain). `random` is two uniform values in
    /// [0, 1) used for scatter; pass fixed values for deterministic replays.
    [[nodiscard]] ImpactResult resolve_impact(const Projectile &projectile, const SurfaceMaterial &material,
                                              const glm::vec3 &entry_point, const glm::vec3 &normal, f32 thickness,
                                              glm::vec2 random) noexcept;

    /// What a ray query reports for the nearest surface along a segment.
    struct SurfaceHit {
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        /// Distance along the ray, in metres, from its origin.
        f32 distance = 0.0f;
        /// Distance the straight ray stays inside the object after this point (from the matching exit hit).
        f32 thickness = 0.0f;
        MaterialId material = no_material;
        /// Opaque id of whatever was hit (entity, body, hitbox) for damage and events.
        u64 object = 0;
    };

    /// Casts a ray and returns the nearest hit within `max_distance`, or false. The caller owns layer filtering.
    using RayQuery = std::function<bool(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, SurfaceHit &hit)>;

    struct ShotImpact {
        SurfaceHit hit;
        ImpactResult result;
    };

    struct ShotOptions {
        f32 max_distance = 1000.0f;
        u32 max_impacts = 8;
        /// Energy below which the projectile is considered spent.
        f32 minimum_energy = 1.0f;
        /// The deterministic stream seed for scatter (a per-shot value shared with the server for replay).
        u64 seed = 1;
    };

    /// Traces a projectile through the world: ray, impact, continue from the exit (or the ricochet) until it stops,
    /// leaves range or runs out of impacts. Hits on materials missing from `surfaces` stop the projectile.
    [[nodiscard]] std::vector<ShotImpact> trace_shot(Projectile projectile, const SurfaceTable &surfaces,
                                                     const RayQuery &query, const ShotOptions &options = {});

} // namespace SFT::Physics
