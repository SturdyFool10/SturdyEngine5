#include <Renderer/Displacement/DisplacementWeld.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include <glm/geometric.hpp>

namespace SFT::Renderer::Displacement {

    namespace {

        struct CellKey {
            i64 x = 0;
            i64 y = 0;
            i64 z = 0;
            [[nodiscard]] bool operator==(const CellKey &) const noexcept = default;
        };

        struct CellKeyHash {
            [[nodiscard]] usize operator()(const CellKey &k) const noexcept {
                usize h = static_cast<usize>(k.x) * 0x9E3779B97F4A7C15ull;
                h ^= static_cast<usize>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
                h ^= static_cast<usize>(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
                return h;
            }
        };

        [[nodiscard]] u32 find_root(vector<u32> &parent, u32 v) noexcept {
            while (parent[v] != v) {
                parent[v] = parent[parent[v]];
                v = parent[v];
            }
            return v;
        }

    } // namespace

    vector<u32> position_weld_groups(span<const GeometryVertex> vertices, f32 weld_epsilon) {
        const auto count = static_cast<u32>(vertices.size());
        vector<u32> parent(count);
        std::iota(parent.begin(), parent.end(), 0u);
        const f32 eps = std::max(weld_epsilon, 1.0e-12f);

        // Cell size = eps, so any two vertices within eps lie in adjacent (3x3x3) cells.
        std::unordered_map<CellKey, vector<u32>, CellKeyHash> cells;
        cells.reserve(vertices.size());
        auto cell_of = [&](const glm::vec3 &p) {
            return CellKey{static_cast<i64>(std::floor(p.x / eps)), static_cast<i64>(std::floor(p.y / eps)),
                           static_cast<i64>(std::floor(p.z / eps))};
        };
        for (u32 i = 0; i < count; ++i) {
            const glm::vec3 &p = vertices[i].position;
            const CellKey c = cell_of(p);
            for (i64 dz = -1; dz <= 1; ++dz) {
                for (i64 dy = -1; dy <= 1; ++dy) {
                    for (i64 dx = -1; dx <= 1; ++dx) {
                        const auto it = cells.find(CellKey{c.x + dx, c.y + dy, c.z + dz});
                        if (it == cells.end()) {
                            continue;
                        }
                        for (const u32 other : it->second) {
                            if (glm::distance(p, vertices[other].position) <= eps) {
                                const u32 a = find_root(parent, i);
                                const u32 b = find_root(parent, other);
                                parent[std::max(a, b)] = std::min(a, b);
                            }
                        }
                    }
                }
            }
            cells[c].push_back(i);
        }
        for (u32 i = 0; i < count; ++i) {
            parent[i] = find_root(parent, i);
        }
        return parent;
    }

    vector<glm::vec3> compute_welded_displacement_normals(span<const GeometryVertex> vertices, span<const u32> indices,
                                                          f32 weld_epsilon) {
        const auto count = static_cast<u32>(vertices.size());
        const vector<u32> group = position_weld_groups(vertices, weld_epsilon);

        // Accumulate in double: the per-group sum is order-dependent in float, and every member of a group
        // must read the same bits.
        vector<glm::dvec3> sum(count, glm::dvec3(0.0));
        for (usize t = 0; t + 2 < indices.size(); t += 3) {
            const u32 idx[3] = {indices[t], indices[t + 1], indices[t + 2]};
            if (idx[0] >= count || idx[1] >= count || idx[2] >= count) {
                continue;
            }
            const glm::dvec3 p[3] = {vertices[idx[0]].position, vertices[idx[1]].position, vertices[idx[2]].position};
            const glm::dvec3 face = glm::cross(p[1] - p[0], p[2] - p[0]);
            const double face_len = glm::length(face);
            if (face_len < 1.0e-20) {
                continue;
            }
            const glm::dvec3 n = face / face_len;
            for (int c = 0; c < 3; ++c) {
                const glm::dvec3 a = p[(c + 1) % 3] - p[c];
                const glm::dvec3 b = p[(c + 2) % 3] - p[c];
                const double la = glm::length(a);
                const double lb = glm::length(b);
                if (la < 1.0e-20 || lb < 1.0e-20) {
                    continue;
                }
                const double cosine = std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0);
                sum[group[idx[c]]] += n * std::acos(cosine);
            }
        }

        // Fallback direction per group: mean of the members' own normals.
        vector<glm::dvec3> own(count, glm::dvec3(0.0));
        for (u32 i = 0; i < count; ++i) {
            own[group[i]] += glm::dvec3(vertices[i].normal);
        }

        vector<glm::vec3> out(count);
        for (u32 i = 0; i < count; ++i) {
            const u32 g = group[i];
            glm::dvec3 v = sum[g];
            if (glm::length(v) < 1.0e-12) {
                v = own[g];
            }
            const double len = glm::length(v);
            out[i] = len > 0.0 ? glm::vec3(v / len) : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        return out;
    }

} // namespace SFT::Renderer::Displacement
