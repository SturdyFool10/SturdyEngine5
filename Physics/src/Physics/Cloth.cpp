#include <Physics/Cloth.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace SFT::Physics {

    Cloth Cloth::grid(u32 columns, u32 rows, f32 spacing, const glm::vec3 &origin, const glm::vec3 &right,
                      const glm::vec3 &down, f32 total_mass) {
        Cloth cloth;
        columns = std::max(columns, 2u);
        rows = std::max(rows, 2u);
        cloth.positions_.reserve(static_cast<usize>(columns) * rows);
        for (u32 y = 0; y < rows; ++y) {
            for (u32 x = 0; x < columns; ++x) {
                cloth.positions_.push_back(origin + right * (spacing * static_cast<f32>(x)) + down * (spacing * static_cast<f32>(y)));
            }
        }
        for (u32 y = 0; y + 1 < rows; ++y) {
            for (u32 x = 0; x + 1 < columns; ++x) {
                const u32 i0 = y * columns + x, i1 = i0 + 1, i2 = i0 + columns, i3 = i2 + 1;
                // Alternate the diagonal so the sheet has no preferred shear direction.
                if (((x + y) & 1u) == 0) {
                    cloth.indices_.insert(cloth.indices_.end(), {i0, i2, i1, i1, i2, i3});
                } else {
                    cloth.indices_.insert(cloth.indices_.end(), {i0, i2, i3, i0, i3, i1});
                }
            }
        }
        const f32 inverse = static_cast<f32>(cloth.positions_.size()) / std::max(total_mass, 1e-6f);
        cloth.inverse_mass_.assign(cloth.positions_.size(), inverse);
        cloth.rest_inverse_mass_ = cloth.inverse_mass_;
        cloth.previous_ = cloth.positions_;
        cloth.velocities_.assign(cloth.positions_.size(), glm::vec3(0.0f));
        cloth.build_constraints();
        return cloth;
    }

    Cloth Cloth::from_triangles(std::span<const glm::vec3> positions, std::span<const u32> indices, f32 total_mass) {
        Cloth cloth;
        cloth.positions_.assign(positions.begin(), positions.end());
        cloth.indices_.assign(indices.begin(), indices.end());
        const f32 inverse = static_cast<f32>(std::max<usize>(cloth.positions_.size(), 1)) / std::max(total_mass, 1e-6f);
        cloth.inverse_mass_.assign(cloth.positions_.size(), inverse);
        cloth.rest_inverse_mass_ = cloth.inverse_mass_;
        cloth.previous_ = cloth.positions_;
        cloth.velocities_.assign(cloth.positions_.size(), glm::vec3(0.0f));
        cloth.build_constraints();
        return cloth;
    }

    void Cloth::build_constraints() {
        stretch_.clear();
        bend_.clear();
        // Edge -> the third vertex of each triangle that owns it (one or two entries).
        std::map<std::pair<u32, u32>, std::pair<u32, u32>> edges;
        constexpr u32 none = 0xFFFFFFFFu;
        const auto add_edge = [&](u32 a, u32 b, u32 opposite) {
            const auto key = std::make_pair(std::min(a, b), std::max(a, b));
            auto [it, inserted] = edges.try_emplace(key, std::make_pair(opposite, none));
            if (!inserted && it->second.second == none) {
                it->second.second = opposite;
            }
        };
        for (usize t = 0; t + 2 < indices_.size(); t += 3) {
            const u32 a = indices_[t], b = indices_[t + 1], c = indices_[t + 2];
            if (a >= positions_.size() || b >= positions_.size() || c >= positions_.size()) {
                continue;
            }
            add_edge(a, b, c);
            add_edge(b, c, a);
            add_edge(c, a, b);
        }
        for (const auto &[edge, opposite] : edges) {
            stretch_.push_back({edge.first, edge.second, glm::length(positions_[edge.first] - positions_[edge.second])});
            if (opposite.second != none) {
                bend_.push_back({opposite.first, opposite.second, glm::length(positions_[opposite.first] - positions_[opposite.second])});
            }
        }
        stretch_lambda_.assign(stretch_.size(), 0.0f);
        bend_lambda_.assign(bend_.size(), 0.0f);
    }

    void Cloth::pin(u32 particle, bool pinned) noexcept {
        if (particle < inverse_mass_.size()) {
            inverse_mass_[particle] = pinned ? 0.0f : rest_inverse_mass_[particle];
            velocities_[particle] = glm::vec3(0.0f);
        }
    }

    bool Cloth::is_pinned(u32 particle) const noexcept {
        return particle < inverse_mass_.size() && inverse_mass_[particle] == 0.0f;
    }

    void Cloth::move_pinned(u32 particle, const glm::vec3 &position) noexcept {
        if (is_pinned(particle)) {
            positions_[particle] = position;
            previous_[particle] = position;
        }
    }

    void Cloth::apply_wind(f32 dt, const ClothParams &params) {
        if (glm::length(params.wind) < 1e-6f || params.drag <= 0.0f) {
            return;
        }
        // Each triangle pushes its vertices along its normal by the wind component through it (a flat plate model).
        for (usize t = 0; t + 2 < indices_.size(); t += 3) {
            const u32 a = indices_[t], b = indices_[t + 1], c = indices_[t + 2];
            const glm::vec3 cross = glm::cross(positions_[b] - positions_[a], positions_[c] - positions_[a]);
            const f32 doubled_area = glm::length(cross);
            if (doubled_area < 1e-9f) {
                continue;
            }
            const glm::vec3 normal = cross / doubled_area;
            const glm::vec3 average_velocity = (velocities_[a] + velocities_[b] + velocities_[c]) / 3.0f;
            const f32 relative = glm::dot(params.wind - average_velocity, normal);
            const glm::vec3 push = normal * (relative * params.drag * 0.5f * doubled_area / 3.0f * dt);
            for (u32 v : {a, b, c}) {
                velocities_[v] += push * inverse_mass_[v];
            }
        }
    }

    void Cloth::collide(const ClothParams &params, const ClothColliders &colliders) {
        for (usize i = 0; i < positions_.size(); ++i) {
            if (inverse_mass_[i] == 0.0f) {
                continue;
            }
            glm::vec3 &p = positions_[i];
            const auto push_out = [&](const glm::vec3 &normal, f32 penetration) {
                p += normal * penetration;
                // Friction: remove part of the tangential displacement this substep.
                const glm::vec3 moved = p - previous_[i];
                const glm::vec3 tangential = moved - normal * glm::dot(moved, normal);
                p -= tangential * params.friction;
            };
            for (const ClothSphere &s : colliders.spheres) {
                const glm::vec3 d = p - s.center;
                const f32 distance = glm::length(d);
                const f32 radius = s.radius + params.collision_thickness;
                if (distance < radius) {
                    const glm::vec3 normal = distance > 1e-7f ? d / distance : glm::vec3(0, 1, 0);
                    push_out(normal, radius - distance);
                }
            }
            for (const ClothCapsule &c : colliders.capsules) {
                const glm::vec3 axis = c.b - c.a;
                const f32 length_squared = glm::dot(axis, axis);
                const f32 t = length_squared > 1e-12f ? std::clamp(glm::dot(p - c.a, axis) / length_squared, 0.0f, 1.0f) : 0.0f;
                const glm::vec3 d = p - (c.a + axis * t);
                const f32 distance = glm::length(d);
                const f32 radius = c.radius + params.collision_thickness;
                if (distance < radius) {
                    const glm::vec3 normal = distance > 1e-7f ? d / distance : glm::vec3(0, 1, 0);
                    push_out(normal, radius - distance);
                }
            }
            for (const ClothPlane &plane : colliders.planes) {
                const f32 distance = glm::dot(p, plane.normal) - plane.offset - params.collision_thickness;
                if (distance < 0.0f) {
                    push_out(plane.normal, -distance);
                }
            }
        }
    }

    void Cloth::step(f32 dt, const ClothParams &params, const ClothColliders &colliders) {
        if (dt <= 0.0f || positions_.empty()) {
            return;
        }
        const u32 substeps = std::max(params.substeps, 1u);
        const f32 h = dt / static_cast<f32>(substeps);
        const f32 keep = std::max(0.0f, 1.0f - params.damping * h);

        const auto solve = [&](const std::vector<Distance> &constraints, std::vector<f32> &lambdas, f32 compliance) {
            const f32 alpha = compliance / (h * h);
            for (usize c = 0; c < constraints.size(); ++c) {
                const Distance &d = constraints[c];
                const f32 wa = inverse_mass_[d.a], wb = inverse_mass_[d.b];
                const f32 w = wa + wb;
                if (w == 0.0f) {
                    continue;
                }
                glm::vec3 delta = positions_[d.a] - positions_[d.b];
                const f32 length = glm::length(delta);
                if (length < 1e-9f) {
                    continue;
                }
                delta /= length;
                const f32 violation = length - d.rest;
                const f32 d_lambda = (-violation - alpha * lambdas[c]) / (w + alpha);
                lambdas[c] += d_lambda;
                positions_[d.a] += delta * (wa * d_lambda);
                positions_[d.b] -= delta * (wb * d_lambda);
            }
        };

        for (u32 s = 0; s < substeps; ++s) {
            apply_wind(h, params);
            for (usize i = 0; i < positions_.size(); ++i) {
                previous_[i] = positions_[i];
                if (inverse_mass_[i] == 0.0f) {
                    continue;
                }
                velocities_[i] += params.gravity * h;
                positions_[i] += velocities_[i] * h;
            }
            std::fill(stretch_lambda_.begin(), stretch_lambda_.end(), 0.0f);
            std::fill(bend_lambda_.begin(), bend_lambda_.end(), 0.0f);
            solve(stretch_, stretch_lambda_, params.stretch_compliance);
            solve(bend_, bend_lambda_, params.bend_compliance);
            collide(params, colliders);
            for (usize i = 0; i < positions_.size(); ++i) {
                if (inverse_mass_[i] == 0.0f) {
                    continue;
                }
                velocities_[i] = (positions_[i] - previous_[i]) / h * keep;
            }
        }
    }

    void Cloth::compute_normals(std::vector<glm::vec3> &out) const {
        out.assign(positions_.size(), glm::vec3(0.0f));
        for (usize t = 0; t + 2 < indices_.size(); t += 3) {
            const u32 a = indices_[t], b = indices_[t + 1], c = indices_[t + 2];
            if (a >= positions_.size() || b >= positions_.size() || c >= positions_.size()) {
                continue;
            }
            const glm::vec3 n = glm::cross(positions_[b] - positions_[a], positions_[c] - positions_[a]);
            out[a] += n;
            out[b] += n;
            out[c] += n;
        }
        for (glm::vec3 &n : out) {
            const f32 length = glm::length(n);
            n = length > 1e-9f ? n / length : glm::vec3(0, 1, 0);
        }
    }

} // namespace SFT::Physics
