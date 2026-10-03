#include <Physics/Ballistics.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>


#include <algorithm>
#include <cmath>

namespace SFT::Physics {

    MaterialId SurfaceTable::add(SurfaceMaterial material) {
        if (const MaterialId existing = find(material.name); existing != no_material) {
            materials_[existing] = std::move(material);
            return existing;
        }
        materials_.push_back(std::move(material));
        return static_cast<MaterialId>(materials_.size() - 1);
    }

    MaterialId SurfaceTable::find(const ustr &name) const noexcept {
        for (auto &&[i, material] : Foundation::iter(materials_).enumerate()) {
            if (material.name == name) {
                return static_cast<MaterialId>(i);
            }
        }
        return no_material;
    }

    const SurfaceMaterial *SurfaceTable::get(MaterialId id) const noexcept {
        return id < materials_.size() ? &materials_[id] : nullptr;
    }

    namespace {

        // Any unit vector perpendicular to `v`.
        glm::vec3 perpendicular(const glm::vec3 &v) noexcept {
            const glm::vec3 helper = std::fabs(v.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            return glm::normalize(glm::cross(v, helper));
        }

        // `direction` turned by up to `max_angle` in a direction chosen by `random` (uniform over the cone's angle).
        glm::vec3 scatter(const glm::vec3 &direction, f32 max_angle, glm::vec2 random) noexcept {
            if (max_angle <= 0.0f) {
                return direction;
            }
            const f32 angle = max_angle * std::sqrt(std::clamp(random.x, 0.0f, 1.0f));
            const f32 around = glm::two_pi<f32>() * std::clamp(random.y, 0.0f, 1.0f);
            const glm::vec3 u = perpendicular(direction);
            const glm::vec3 v = glm::cross(direction, u);
            const glm::vec3 axis = u * std::cos(around) + v * std::sin(around);
            return glm::normalize(glm::angleAxis(angle, axis) * direction);
        }

        // splitmix64: a tiny, platform-independent stream for scatter, so a shot replays identically everywhere.
        struct Stream {
            u64 state;
            u64 next() noexcept {
                u64 z = (state += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }
            f32 uniform() noexcept { return static_cast<f32>(next() >> 40) * (1.0f / 16777216.0f); }
        };

    } // namespace

    ImpactResult resolve_impact(const Projectile &projectile, const SurfaceMaterial &material, const glm::vec3 &entry_point,
                                const glm::vec3 &normal, f32 thickness, glm::vec2 random) noexcept {
        ImpactResult result;
        result.point = entry_point;
        const glm::vec3 direction = glm::normalize(projectile.direction);
        // Angle between the ray and the surface plane: 90 degrees is a square hit, 0 is skimming.
        const f32 cos_incidence = std::clamp(-glm::dot(direction, normal), 0.0f, 1.0f);
        const f32 grazing = glm::degrees(std::asin(cos_incidence));

        if (grazing < material.ricochet_angle_degrees) {
            const glm::vec3 reflected = glm::reflect(direction, normal);
            // The closer to skimming, the tighter the bounce; at the threshold it scatters the most.
            const f32 closeness = material.ricochet_angle_degrees > 0.0f ? grazing / material.ricochet_angle_degrees : 1.0f;
            result.outcome = ImpactOutcome::Ricocheted;
            result.direction = scatter(reflected, material.ricochet_scatter * closeness, random);
            // A scattered bounce must still leave the surface.
            if (glm::dot(result.direction, normal) < 0.0f) {
                result.direction = glm::reflect(result.direction, normal);
            }
            result.remaining_energy = projectile.energy * std::clamp(material.ricochet_energy_retained, 0.0f, 1.0f);
            return result;
        }

        const f32 resistance = std::max(material.penetration_resistance, 1e-3f);
        const f32 max_depth = projectile.energy / resistance;
        // Moving obliquely through a slab means a longer path than its thickness; `thickness` is already measured
        // along the ray, so it is compared directly.
        if (thickness <= max_depth) {
            result.outcome = ImpactOutcome::Penetrated;
            result.depth = thickness;
            result.point = entry_point + direction * thickness;
            result.remaining_energy = projectile.energy - resistance * thickness;
            const f32 strain = max_depth > 0.0f ? thickness / max_depth : 1.0f;
            result.direction = scatter(direction, material.exit_deflection * strain, random);
        } else {
            result.outcome = ImpactOutcome::Stopped;
            result.depth = max_depth;
            result.point = entry_point + direction * max_depth;
            result.remaining_energy = 0.0f;
            result.direction = direction;
        }
        return result;
    }

    std::vector<ShotImpact> trace_shot(Projectile projectile, const SurfaceTable &surfaces, const RayQuery &query,
                                       const ShotOptions &options) {
        std::vector<ShotImpact> impacts;
        Stream stream{options.seed};
        f32 remaining_distance = options.max_distance;
        projectile.direction = glm::normalize(projectile.direction);

        while (impacts.size() < options.max_impacts && projectile.energy > options.minimum_energy && remaining_distance > 0.0f) {
            SurfaceHit hit;
            if (!query(projectile.position, projectile.direction, remaining_distance, hit)) {
                break;
            }
            const SurfaceMaterial *material = surfaces.get(hit.material);
            ShotImpact impact;
            impact.hit = hit;
            if (material == nullptr) {
                impact.result = ImpactResult{.outcome = ImpactOutcome::Stopped, .point = hit.point, .direction = projectile.direction};
                impacts.push_back(impact);
                break;
            }
            const glm::vec2 random{stream.uniform(), stream.uniform()};
            impact.result = resolve_impact(projectile, *material, hit.point, hit.normal, hit.thickness, random);
            impacts.push_back(impact);
            if (impact.result.outcome == ImpactOutcome::Stopped) {
                break;
            }
            remaining_distance -= hit.distance;
            if (impact.result.outcome == ImpactOutcome::Penetrated) {
                remaining_distance -= impact.result.depth;
            }
            // Step off the surface so the next ray does not re-hit it.
            const f32 skin = 1e-3f;
            projectile.position = impact.result.point + impact.result.direction * skin;
            projectile.direction = impact.result.direction;
            projectile.energy = impact.result.remaining_energy;
        }
        return impacts;
    }

} // namespace SFT::Physics
