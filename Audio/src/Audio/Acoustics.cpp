#include <Audio/Acoustics.hpp>

#include <Async/ParIter.hpp>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <ranges>
#include <cstring>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace SFT::Audio {

    // ---- materials -----------------------------------------------------------------------------------------------

    AcousticMaterialId AcousticMaterialTable::add(AcousticMaterial material) {
        if (const AcousticMaterialId existing = find(material.name); existing != no_acoustic_material) {
            materials_[existing] = std::move(material);
            return existing;
        }
        materials_.push_back(std::move(material));
        return static_cast<AcousticMaterialId>(materials_.size() - 1);
    }

    AcousticMaterialId AcousticMaterialTable::find(const ustr &name) const noexcept {
        const auto index = Foundation::iter(materials_).position([&](const AcousticMaterial &material) { return material.name == name; });
        return index ? static_cast<AcousticMaterialId>(*index) : no_acoustic_material;
    }

    const AcousticMaterial *AcousticMaterialTable::get(AcousticMaterialId id) const noexcept {
        return id < materials_.size() ? &materials_[id] : nullptr;
    }

    AcousticMaterialTable AcousticMaterialTable::with_defaults() {
        AcousticMaterialTable t;
        t.add({"concrete", {0.01f, 0.02f, 0.05f}, {0.02f, 0.005f, 0.001f}, 0.1f});
        t.add({"brick", {0.02f, 0.03f, 0.05f}, {0.03f, 0.01f, 0.002f}, 0.25f});
        t.add({"wood", {0.15f, 0.10f, 0.08f}, {0.25f, 0.12f, 0.05f}, 0.15f});
        t.add({"glass", {0.18f, 0.06f, 0.03f}, {0.40f, 0.25f, 0.10f}, 0.05f});
        t.add({"drywall", {0.15f, 0.08f, 0.09f}, {0.40f, 0.15f, 0.05f}, 0.1f});
        t.add({"carpet", {0.08f, 0.30f, 0.60f}, {0.70f, 0.50f, 0.30f}, 0.6f});
        t.add({"metal", {0.02f, 0.03f, 0.03f}, {0.02f, 0.005f, 0.002f}, 0.05f});
        t.add({"fabric", {0.10f, 0.40f, 0.70f}, {0.80f, 0.60f, 0.40f}, 0.7f});
        t.add({"foliage", {0.10f, 0.30f, 0.50f}, {0.90f, 0.80f, 0.60f}, 0.9f});
        return t;
    }

    // ---- triangle BVH ----------------------------------------------------------------------------------------------

    void TriangleBvh::clear() {
        added_triangles_ = 0;
        packed_.clear();
        triangles_.clear();
        order_.clear();
        nodes_.clear();
    }

    void TriangleBvh::add_mesh(std::span<const glm::vec3> positions, std::span<const u32> indices, AcousticMaterialId material,
                               const glm::mat4 &transform) {
        const auto to_world = [&](const glm::vec3 &p) { return glm::vec3(transform * glm::vec4(p, 1.0f)); };
        for (usize i = 0; i + 2 < indices.size(); i += 3) {
            const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a >= positions.size() || b >= positions.size() || c >= positions.size()) {
                continue;
            }
            triangles_.push_back({to_world(positions[a]), to_world(positions[b]), to_world(positions[c]), material});
            ++added_triangles_;
        }
    }

    namespace {

        constexpr usize kParallelTriangleThreshold = 8192;

        f32 triangle_area(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c) { return 0.5f * glm::length(glm::cross(b - a, c - a)); }

    } // namespace

    // Merges clusters of small triangles (same material, facing the same way, in the same cell of a grid as coarse as the
    // requested feature size) into one oriented quad each, then enforces the triangle budget by dropping the smallest.
    void TriangleBvh::simplify() {
        std::erase_if(triangles_, [](const Triangle &t) { return triangle_area(t.a, t.b, t.c) < 1e-9f; });
        const f32 size = settings_.voxel_size;
        if (size > 0.0f) {
            struct Cluster {
                glm::vec3 normal_sum{0.0f};
                std::vector<glm::vec3> points;
                std::vector<Triangle> originals;
                AcousticMaterialId material = no_acoustic_material;
            };
            std::map<std::array<i64, 5>, Cluster> clusters;
            std::vector<Triangle> kept;
            for (const Triangle &t : triangles_) {
                const f32 longest = std::max({glm::length(t.b - t.a), glm::length(t.c - t.b), glm::length(t.a - t.c)});
                if (longest >= size) {
                    kept.push_back(t);
                    continue;
                }
                const glm::vec3 centroid = (t.a + t.b + t.c) / 3.0f;
                glm::vec3 n = glm::cross(t.b - t.a, t.c - t.a);
                const f32 area2 = glm::length(n);
                n = n / area2;
                // Which of six facings the triangle is closest to, so opposite faces of a thin object stay separate.
                const int axis = std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z) ? 0 : (std::fabs(n.y) >= std::fabs(n.z) ? 1 : 2);
                const i64 facing = axis * 2 + (n[axis] < 0.0f ? 1 : 0);
                const std::array<i64, 5> key{static_cast<i64>(std::floor(centroid.x / size)), static_cast<i64>(std::floor(centroid.y / size)),
                                              static_cast<i64>(std::floor(centroid.z / size)), static_cast<i64>(t.material), facing};
                Cluster &c = clusters[key];
                c.material = t.material;
                c.normal_sum += n * area2;
                c.points.insert(c.points.end(), {t.a, t.b, t.c});
                c.originals.push_back(t);
            }
            for (auto &[key, c] : clusters) {
                const f32 length = glm::length(c.normal_sum);
                if (c.originals.size() < 2 || length < 1e-9f) {
                    kept.insert(kept.end(), c.originals.begin(), c.originals.end());
                    continue;
                }
                const glm::vec3 n = c.normal_sum / length;
                const glm::vec3 helper = std::fabs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
                const glm::vec3 u = glm::normalize(glm::cross(n, helper)), v = glm::cross(n, u);
                glm::vec3 centre(0.0f);
                for (const glm::vec3 &p : c.points) centre += p;
                centre /= static_cast<f32>(c.points.size());
                f32 umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
                for (const glm::vec3 &p : c.points) {
                    const f32 pu = glm::dot(p - centre, u), pv = glm::dot(p - centre, v);
                    umin = std::min(umin, pu); umax = std::max(umax, pu);
                    vmin = std::min(vmin, pv); vmax = std::max(vmax, pv);
                }
                const glm::vec3 p00 = centre + u * umin + v * vmin, p10 = centre + u * umax + v * vmin;
                const glm::vec3 p11 = centre + u * umax + v * vmax, p01 = centre + u * umin + v * vmax;
                kept.push_back({p00, p10, p11, c.material});
                kept.push_back({p00, p11, p01, c.material});
            }
            triangles_ = std::move(kept);
        }
        if (settings_.max_triangles > 0 && triangles_.size() > settings_.max_triangles) {
            std::nth_element(triangles_.begin(), triangles_.begin() + settings_.max_triangles, triangles_.end(), [](const Triangle &x, const Triangle &y) {
                return triangle_area(x.a, x.b, x.c) > triangle_area(y.a, y.b, y.c);
            });
            triangles_.resize(settings_.max_triangles);
        }
    }

    void TriangleBvh::build() {
        simplify();
        nodes_.clear();
        order_.resize(triangles_.size());
        std::iota(order_.begin(), order_.end(), 0u);
        if (triangles_.empty()) {
            return;
        }
        nodes_.reserve(triangles_.size() / std::max(settings_.max_leaf_triangles, 1u) * 2 + 2);
        nodes_.emplace_back();
        fill_node(0, 0, static_cast<u32>(triangles_.size()), 0);
        // Lay the triangles out in leaf order with their edges precomputed: traversal then reads one contiguous run per leaf.
        packed_.resize(order_.size());
        const auto pack_triangle = [&](usize i) {
            const Triangle &t = triangles_[order_[i]];
            packed_[i] = PackedTriangle{t.a, t.b - t.a, t.c - t.a, t.material};
        };
        if (order_.size() >= kParallelTriangleThreshold) {
            Async::par_iter(std::views::iota(usize{0}, order_.size())).for_each(pack_triangle);
        } else {
            for (usize i = 0; i < order_.size(); ++i) {
                pack_triangle(i);
            }
        }
    }

    void TriangleBvh::fill_node(u32 index, u32 first, u32 count, u32 depth) {
        glm::vec3 lo(std::numeric_limits<f32>::max()), hi(std::numeric_limits<f32>::lowest());
        glm::vec3 centroid_lo = lo, centroid_hi = hi;
        for (u32 i = first; i < first + count; ++i) {
            const Triangle &t = triangles_[order_[i]];
            for (const glm::vec3 &p : {t.a, t.b, t.c}) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
            const glm::vec3 centroid = (t.a + t.b + t.c) / 3.0f;
            centroid_lo = glm::min(centroid_lo, centroid);
            centroid_hi = glm::max(centroid_hi, centroid);
        }
        nodes_[index].min = lo;
        nodes_[index].max = hi;

        const u32 leaf_size = std::max(settings_.max_leaf_triangles, 1u);
        const glm::vec3 extent = centroid_hi - centroid_lo;
        if (count <= leaf_size || depth >= 56 || glm::dot(extent, extent) < 1e-12f) {
            nodes_[index].left_or_first = first;
            nodes_[index].count = count;
            return;
        }
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        const auto centroid_on_axis = [&](u32 tri) {
            const Triangle &t = triangles_[tri];
            return (t.a[axis] + t.b[axis] + t.c[axis]) / 3.0f;
        };
        u32 middle = first + count / 2;
        bool split_done = false;
        if (settings_.split == BvhSplit::Sah && settings_.sah_bins >= 2) {
            // Binned SAH: choose the plane that minimises (left area * left count + right area * right count).
            const u32 bins = std::min(settings_.sah_bins, 64u);
            struct Bin { glm::vec3 lo{1e30f}, hi{-1e30f}; u32 n = 0; };
            std::vector<Bin> bin(bins);
            const f32 span = extent[axis];
            const auto bin_of = [&](u32 tri) { return std::min(bins - 1, static_cast<u32>((centroid_on_axis(tri) - centroid_lo[axis]) / span * static_cast<f32>(bins))); };
            for (u32 i = first; i < first + count; ++i) {
                const Triangle &t = triangles_[order_[i]];
                Bin &b = bin[bin_of(order_[i])];
                for (const glm::vec3 &p : {t.a, t.b, t.c}) { b.lo = glm::min(b.lo, p); b.hi = glm::max(b.hi, p); }
                ++b.n;
            }
            const auto area = [](const glm::vec3 &l, const glm::vec3 &h) { const glm::vec3 d = h - l; return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x); };
            f32 best_cost = std::numeric_limits<f32>::max();
            u32 best_split = 0;
            for (u32 split = 1; split < bins; ++split) {
                glm::vec3 llo(1e30f), lhi(-1e30f), rlo(1e30f), rhi(-1e30f);
                u32 ln = 0, rn = 0;
                for (u32 b = 0; b < split; ++b) if (bin[b].n) { llo = glm::min(llo, bin[b].lo); lhi = glm::max(lhi, bin[b].hi); ln += bin[b].n; }
                for (u32 b = split; b < bins; ++b) if (bin[b].n) { rlo = glm::min(rlo, bin[b].lo); rhi = glm::max(rhi, bin[b].hi); rn += bin[b].n; }
                if (ln == 0 || rn == 0) continue;
                const f32 cost = area(llo, lhi) * static_cast<f32>(ln) + area(rlo, rhi) * static_cast<f32>(rn);
                if (cost < best_cost) { best_cost = cost; best_split = split; }
            }
            if (best_split != 0) {
                const auto mid = std::partition(order_.begin() + first, order_.begin() + first + count, [&](u32 tri) { return bin_of(tri) < best_split; });
                middle = static_cast<u32>(mid - order_.begin());
                split_done = middle > first && middle < first + count;
            }
        }
        if (!split_done) {
            middle = first + count / 2;
            std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + first + count,
                             [&](u32 a, u32 b) { return centroid_on_axis(a) < centroid_on_axis(b); });
        }

        const u32 left = static_cast<u32>(nodes_.size());
        nodes_.emplace_back();
        nodes_.emplace_back();
        nodes_[index].left_or_first = left;
        nodes_[index].count = 0;
        fill_node(left, first, middle - first, depth + 1);
        fill_node(left + 1, middle, first + count - middle, depth + 1);
    }

    template <bool AnyHit>
    bool TriangleBvh::traverse(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, AudioRayHit *hit) const {
        if (nodes_.empty()) {
            return false;
        }
        const auto safe_inverse = [](f32 d) { return 1.0f / (std::fabs(d) < 1e-12f ? (d < 0.0f ? -1e-12f : 1e-12f) : d); };
        const glm::vec3 inverse(safe_inverse(direction.x), safe_inverse(direction.y), safe_inverse(direction.z));
        f32 best = max_distance;

        // Distance at which the ray enters a node's box, or a value beyond `best` when it misses (or the box is farther away).
        const auto entry = [&](const Node &node) {
            f32 t_min = 0.0f, t_max = best;
            for (int axis = 0; axis < 3; ++axis) {
                f32 t0 = (node.min[axis] - origin[axis]) * inverse[axis];
                f32 t1 = (node.max[axis] - origin[axis]) * inverse[axis];
                if (t0 > t1) {
                    std::swap(t0, t1);
                }
                t_min = std::max(t_min, t0);
                t_max = std::min(t_max, t1);
            }
            return t_max >= t_min ? t_min : std::numeric_limits<f32>::max();
        };

        std::array<u32, 64> stack;
        std::array<f32, 64> stack_entry;
        u32 top = 0;
        if (entry(nodes_[0]) == std::numeric_limits<f32>::max()) {
            return false;
        }
        stack[top] = 0;
        stack_entry[top++] = 0.0f;
        bool found = false;
        u32 best_triangle = 0;

        while (top > 0) {
            --top;
            if (stack_entry[top] > best) {
                continue; // pushed before a closer hit was found
            }
            const Node &node = nodes_[stack[top]];
            if (node.count > 0) {
                for (u32 i = node.left_or_first; i < node.left_or_first + node.count; ++i) {
                    const PackedTriangle &tri = packed_[i];
                    // Moller-Trumbore, double sided.
                    const glm::vec3 p = glm::cross(direction, tri.e2);
                    const f32 det = glm::dot(tri.e1, p);
                    if (std::fabs(det) < 1e-12f) {
                        continue;
                    }
                    const f32 inv_det = 1.0f / det;
                    const glm::vec3 tv = origin - tri.a;
                    const f32 u = glm::dot(tv, p) * inv_det;
                    if (u < 0.0f || u > 1.0f) {
                        continue;
                    }
                    const glm::vec3 q = glm::cross(tv, tri.e1);
                    const f32 v = glm::dot(direction, q) * inv_det;
                    if (v < 0.0f || u + v > 1.0f) {
                        continue;
                    }
                    const f32 t = glm::dot(tri.e2, q) * inv_det;
                    if (t > 1e-4f && t < best) {
                        if constexpr (AnyHit) {
                            return true;
                        }
                        best = t;
                        best_triangle = i;
                        found = true;
                    }
                }
            } else if (top + 2 <= stack.size()) {
                const u32 left = node.left_or_first, right = node.left_or_first + 1;
                const f32 t_left = entry(nodes_[left]), t_right = entry(nodes_[right]);
                // Push the farther child first so the nearer one is examined next: a close hit then prunes the far subtree.
                if (t_left <= t_right) {
                    if (t_right != std::numeric_limits<f32>::max()) { stack[top] = right; stack_entry[top++] = t_right; }
                    if (t_left != std::numeric_limits<f32>::max()) { stack[top] = left; stack_entry[top++] = t_left; }
                } else {
                    if (t_left != std::numeric_limits<f32>::max()) { stack[top] = left; stack_entry[top++] = t_left; }
                    if (t_right != std::numeric_limits<f32>::max()) { stack[top] = right; stack_entry[top++] = t_right; }
                }
            }
        }
        if constexpr (!AnyHit) {
            if (found && hit != nullptr) {
                const PackedTriangle &tri = packed_[best_triangle];
                glm::vec3 normal = glm::normalize(glm::cross(tri.e1, tri.e2));
                if (glm::dot(normal, direction) > 0.0f) {
                    normal = -normal;
                }
                *hit = AudioRayHit{best, normal, tri.material};
            }
        }
        return found;
    }

    bool TriangleBvh::closest_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance, AudioRayHit &hit) const {
        return traverse<false>(origin, direction, max_distance, &hit);
    }

    bool TriangleBvh::any_hit(const glm::vec3 &origin, const glm::vec3 &direction, f32 max_distance) const {
        return traverse<true>(origin, direction, max_distance, nullptr);
    }

    // ---- providers -------------------------------------------------------------------------------------------------

    void NullAcoustics::evaluate(std::span<const AcousticsQuery> queries, std::span<AcousticsResult> results) {
        for (usize i = 0; i < queries.size() && i < results.size(); ++i) {
            results[i] = AcousticsResult{};
        }
    }

    namespace {

        constexpr f32 kGolden = 2.3999632f;

        RaycastAcousticsSettings with_legacy_names(RaycastAcousticsSettings s) {
            if (s.occlusion_rays != 0) s.muffling.occlusion_rays = s.occlusion_rays;
            if (s.diffraction_probes != 0) s.muffling.diffraction_probes = s.diffraction_probes;
            if (s.room_rays != 0) s.reverb.room_rays = s.room_rays;
            if (s.max_crossings != 0) s.muffling.max_crossings = s.max_crossings;
            return s;
        }

        // Deterministic hash to [0, 1): picks scatter directions without a random number generator's state.
        f32 hash01(u32 a, u32 b, u32 c) {
            u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
            h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
            return static_cast<f32>(h & 0xFFFFFFu) / 16777216.0f;
        }

        glm::vec3 fibonacci_direction(u32 i, u32 count, f32 phase) {
            const f32 y = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(count);
            const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const f32 phi = static_cast<f32>(i) * kGolden + phase;
            return glm::vec3(std::cos(phi) * r, y, std::sin(phi) * r);
        }

        glm::vec3 cosine_hemisphere(const glm::vec3 &normal, f32 r1, f32 r2) {
            const glm::vec3 helper = std::fabs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 u = glm::normalize(glm::cross(normal, helper)), v = glm::cross(normal, u);
            const f32 radius = std::sqrt(r1), angle = glm::two_pi<f32>() * r2;
            return glm::normalize(u * (radius * std::cos(angle)) + v * (radius * std::sin(angle)) + normal * std::sqrt(std::max(0.0f, 1.0f - r1)));
        }

        f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

    } // namespace

    // One reflection point on a ray cast from the listener, with the sound energy that has survived to it.
    struct RaycastAcoustics::CachedVertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec3 first_direction; // direction of the ray as it left the listener: where this sound arrives from
        std::array<f32, acoustic_bands> energy;
        f32 length;                // listener to this point along the ray
        f32 scattering;
        u32 bounce;                // 1 = the surface the ray hit first
        AcousticMaterialId material;
    };


    RaycastAcoustics::RaycastAcoustics(std::shared_ptr<const AudioRayScene> scene, std::shared_ptr<const AcousticMaterialTable> materials,
                                       const RaycastAcousticsSettings &settings)
        : scene_(std::move(scene)), materials_(std::move(materials)), settings_(with_legacy_names(settings)) {}

    void RaycastAcoustics::set_settings(const RaycastAcousticsSettings &settings) {
        settings_ = with_legacy_names(settings);
        cached_.reset(); // the rays depend on the settings
    }

    bool RaycastAcoustics::listener_room(RoomEstimate &out) const {
        if (!have_room_) {
            return false;
        }
        out = last_room_;
        return true;
    }

    bool RaycastAcoustics::clear_between(const glm::vec3 &from, const glm::vec3 &to) const {
        const glm::vec3 delta = to - from;
        const f32 length = glm::length(delta);
        if (length < 1e-4f) {
            return true;
        }
        return !scene_->any_hit(from, delta / length, length - 2e-3f);
    }

    std::array<f32, acoustic_bands> RaycastAcoustics::transmission_along(const glm::vec3 &from, const glm::vec3 &to, u32 &crossings) const {
        std::array<f32, acoustic_bands> transmission{1.0f, 1.0f, 1.0f};
        crossings = 0;
        const glm::vec3 delta = to - from;
        const f32 length = glm::length(delta);
        if (length < 1e-4f) {
            return transmission;
        }
        const glm::vec3 direction = delta / length;
        glm::vec3 position = from;
        f32 remaining = length;
        while (remaining > 1e-3f) {
            if (crossings >= settings_.muffling.max_crossings) {
                transmission.fill(0.0f); // too many walls: treat as sealed
                return transmission;
            }
            AudioRayHit hit;
            if (!scene_->closest_hit(position, direction, remaining, hit)) {
                break;
            }
            ++crossings;
            const AcousticMaterial *material = materials_ ? materials_->get(hit.material) : nullptr;
            for (u32 band = 0; band < acoustic_bands; ++band) {
                transmission[band] *= material != nullptr ? material->transmission[band] : 0.3f;
            }
            const f32 advance = hit.distance + 1e-3f;
            position += direction * advance;
            remaining -= advance;
        }
        return transmission;
    }

    RoomEstimate RaycastAcoustics::estimate_room(const glm::vec3 &position) const {
        RoomEstimate room;
        const u32 rays = std::max(settings_.reverb.room_rays, 8u);
        u32 hits = 0;
        f64 distance_sum = 0.0;
        std::array<f64, acoustic_bands> absorption_sum{0.0, 0.0, 0.0};
        for (u32 i = 0; i < rays; ++i) {
            const glm::vec3 direction = fibonacci_direction(i, rays, 0.0f);
            AudioRayHit hit;
            if (scene_->closest_hit(position, direction, settings_.max_distance, hit)) {
                ++hits;
                distance_sum += hit.distance;
                const AcousticMaterial *material = materials_ ? materials_->get(hit.material) : nullptr;
                for (u32 b = 0; b < acoustic_bands; ++b) {
                    absorption_sum[b] += material != nullptr ? material->absorption[b] : 0.2f;
                }
            }
        }
        room.openness = 1.0f - static_cast<f32>(hits) / static_cast<f32>(rays);
        if (hits > 0) {
            room.mean_distance = static_cast<f32>(distance_sum / hits);
            for (u32 b = 0; b < acoustic_bands; ++b) {
                const f32 absorption = std::clamp(static_cast<f32>(absorption_sum[b] / hits), 0.01f, 0.99f);
                // Eyring: RT60 = 0.161 V / (-S ln(1 - a)), with V/S = mean free path / 4 for a diffuse enclosure.
                const f32 enclosed = 0.161f * (room.mean_distance * 0.25f) / (-std::log(1.0f - absorption));
                room.rt60_bands[b] = std::min(enclosed, 10.0f) * (1.0f - room.openness);
            }
            room.rt60 = room.rt60_bands[1];
        }
        return room;
    }

    std::shared_ptr<const AcousticField> AcousticField::bake(const RaycastAcoustics &acoustics, const Settings &settings) {
        std::shared_ptr<AcousticField> field(new AcousticField());
        field->settings_ = settings;
        field->settings_.cell_size = std::max(settings.cell_size, 0.1f);
        const glm::vec3 extent = glm::max(settings.max - settings.min, glm::vec3(0.0f));
        field->dims_ = glm::uvec3(glm::max(glm::ivec3(glm::ceil(extent / field->settings_.cell_size)) + 1, glm::ivec3(2)));
        field->cells_.resize(static_cast<usize>(field->dims_.x) * field->dims_.y * field->dims_.z);
        const AcousticField &f = *field;
        // Every cell is independent and the scene is read-only: one task per slab of cells.
        Async::par_iter(std::views::iota(usize{0}, field->cells_.size()))
            .for_each([&acoustics, &f, cells = field->cells_.data()](usize i) {
                const u32 x = static_cast<u32>(i % f.dims_.x), y = static_cast<u32>((i / f.dims_.x) % f.dims_.y), z = static_cast<u32>(i / (static_cast<usize>(f.dims_.x) * f.dims_.y));
                cells[i] = acoustics.estimate_room(f.settings_.min + glm::vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)) * f.settings_.cell_size);
            });
        return field;
    }

    bool AcousticField::lookup(const glm::vec3 &position, RoomEstimate &out) const noexcept {
        if (cells_.empty()) return false;
        const glm::vec3 grid = (position - settings_.min) / settings_.cell_size;
        if (grid.x < 0.0f || grid.y < 0.0f || grid.z < 0.0f || grid.x > static_cast<f32>(dims_.x - 1) || grid.y > static_cast<f32>(dims_.y - 1) || grid.z > static_cast<f32>(dims_.z - 1)) {
            return false;
        }
        const glm::uvec3 base = glm::min(glm::uvec3(glm::floor(grid)), dims_ - glm::uvec3(2u));
        const glm::vec3 t = grid - glm::vec3(base);
        RoomEstimate result{};
        for (u32 corner = 0; corner < 8; ++corner) {
            const u32 dx = corner & 1, dy = (corner >> 1) & 1, dz = (corner >> 2) & 1;
            const f32 w = (dx ? t.x : 1.0f - t.x) * (dy ? t.y : 1.0f - t.y) * (dz ? t.z : 1.0f - t.z);
            const RoomEstimate &c = at(base.x + dx, base.y + dy, base.z + dz);
            result.mean_distance += w * c.mean_distance;
            result.openness += 0.0f;
            result.rt60 += w * c.rt60;
            for (u32 b = 0; b < acoustic_bands; ++b) result.rt60_bands[b] += w * c.rt60_bands[b];
        }
        result.openness = 0.0f;
        for (u32 corner = 0; corner < 8; ++corner) {
            const u32 dx = corner & 1, dy = (corner >> 1) & 1, dz = (corner >> 2) & 1;
            result.openness += (dx ? t.x : 1.0f - t.x) * (dy ? t.y : 1.0f - t.y) * (dz ? t.z : 1.0f - t.z) * at(base.x + dx, base.y + dy, base.z + dz).openness;
        }
        out = result;
        return true;
    }

    std::vector<std::byte> AcousticField::serialize() const {
        std::vector<std::byte> out;
        const auto put = [&out](const void *data, usize size) {
            const auto *p = static_cast<const std::byte *>(data);
            out.insert(out.end(), p, p + size);
        };
        const char magic[4] = {'S', 'A', 'F', 'D'};
        const u32 version = 1;
        put(magic, 4);
        put(&version, 4);
        put(&settings_.min, sizeof(glm::vec3));
        put(&settings_.max, sizeof(glm::vec3));
        put(&settings_.cell_size, 4);
        put(&dims_, sizeof(glm::uvec3));
        for (const RoomEstimate &c : cells_) {
            const f32 values[6] = {c.mean_distance, c.openness, c.rt60, c.rt60_bands[0], c.rt60_bands[1], c.rt60_bands[2]};
            put(values, sizeof(values));
        }
        return out;
    }

    std::expected<std::shared_ptr<const AcousticField>, UString> AcousticField::deserialize(std::span<const std::byte> bytes) {
        usize pos = 0;
        const auto take = [&](void *dst, usize size) {
            if (pos + size > bytes.size()) return false;
            std::memcpy(dst, bytes.data() + pos, size);
            pos += size;
            return true;
        };
        char magic[4];
        u32 version = 0;
        std::shared_ptr<AcousticField> field(new AcousticField());
        if (!take(magic, 4) || std::memcmp(magic, "SAFD", 4) != 0 || !take(&version, 4) || version != 1) return std::unexpected("audio: not an acoustic field (or a newer version)");
        if (!take(&field->settings_.min, sizeof(glm::vec3)) || !take(&field->settings_.max, sizeof(glm::vec3)) || !take(&field->settings_.cell_size, 4) || !take(&field->dims_, sizeof(glm::uvec3))) {
            return std::unexpected("audio: the acoustic field is truncated");
        }
        const u64 count = static_cast<u64>(field->dims_.x) * field->dims_.y * field->dims_.z;
        if (field->dims_.x < 2 || field->dims_.y < 2 || field->dims_.z < 2 || count > (u64{1} << 28) || (bytes.size() - pos) / 24 < count) return std::unexpected("audio: the acoustic field is damaged");
        field->cells_.resize(static_cast<usize>(count));
        for (RoomEstimate &c : field->cells_) {
            f32 values[6];
            take(values, sizeof(values));
            c = RoomEstimate{values[0], values[1], values[2], {values[3], values[4], values[5]}};
        }
        return field;
    }

    void RaycastAcoustics::evaluate(std::span<const AcousticsQuery> queries, std::span<AcousticsResult> results) {
        if (queries.empty() || !scene_) {
            return;
        }
        const RaycastAcousticsSettings &cfg = settings_;
        const glm::vec3 listener = queries.front().listener; // queries from one pass share a listener

        // ---- the listener's side of every path, traced once for all sources (and kept while the listener stays put) -------------
        const bool reuse = cfg.cache.enabled && cached_ && have_room_ && cache_age_ < cfg.cache.max_age &&
                           glm::distance(listener, cached_listener_) <= cfg.cache.listener_distance;
        RoomEstimate room;
        if (reuse) {
            ++cache_age_;
            room = last_room_;
        } else {
            if (!baked_field_ || !baked_field_->lookup(listener, room)) {
                room = estimate_room(listener);
            }
            last_room_ = room;
            have_room_ = true;
            cached_listener_ = listener;
            cache_age_ = 0;
            ++listener_traces_;
            cached_ = std::make_shared<std::vector<CachedVertex>>();
        }
        std::vector<CachedVertex> &vertices = *cached_;
        const bool need_paths = cfg.bounces.enabled || cfg.directionality.enabled;
        const u32 ray_count = std::max(cfg.bounces.rays, 1u);
        if (need_paths && !reuse) {
            ++jitter_counter_;
            const f32 phase = cfg.bounces.jitter ? static_cast<f32>(jitter_counter_) * 0.61803f : 0.0f;
            vertices.reserve(static_cast<usize>(ray_count) * cfg.bounces.max_bounces);
            for (u32 i = 0; i < ray_count; ++i) {
                const glm::vec3 first = fibonacci_direction(i, ray_count, phase);
                glm::vec3 origin = listener, direction = first;
                std::array<f32, acoustic_bands> energy{1.0f, 1.0f, 1.0f};
                f32 length = 0.0f;
                for (u32 bounce = 1; bounce <= std::max(cfg.bounces.max_bounces, 1u); ++bounce) {
                    AudioRayHit hit;
                    if (!scene_->closest_hit(origin, direction, cfg.max_distance - length, hit)) {
                        break; // the ray escaped into open air
                    }
                    length += hit.distance;
                    const glm::vec3 point = origin + direction * hit.distance + hit.normal * 2e-3f;
                    const AcousticMaterial *material = materials_ ? materials_->get(hit.material) : nullptr;
                    f32 scatter = 0.3f;
                    for (u32 b = 0; b < acoustic_bands; ++b) {
                        energy[b] *= material != nullptr ? material->reflectivity(b) : 0.6f;
                    }
                    if (material != nullptr) scatter = material->scattering;
                    vertices.push_back(CachedVertex{point, hit.normal, first, energy, length, scatter, bounce, hit.material});
                    if (*std::max_element(energy.begin(), energy.end()) < cfg.bounces.min_energy) {
                        break;
                    }
                    // Continue: a mirror bounce, or (with probability `scattering`) a diffuse one.
                    const bool diffuse = hash01(i, bounce, jitter_counter_) < scatter;
                    direction = diffuse ? cosine_hemisphere(hit.normal, hash01(i, bounce, 7u), hash01(i, bounce, 13u))
                                        : glm::normalize(direction - 2.0f * glm::dot(direction, hit.normal) * hit.normal);
                    origin = point;
                }
            }
        }

        for (usize qi = 0; qi < queries.size() && qi < results.size(); ++qi) {
            const AcousticsQuery &q = queries[qi];
            const auto on = [&](AcousticEffect e) { return (q.enabled_effects & static_cast<u32>(e)) != 0; };
            const bool muffling_on = cfg.muffling.enabled && on(AcousticEffect::Muffling) && q.muffling_scale > 0.0f;
            const bool bounces_on = cfg.bounces.enabled && on(AcousticEffect::MaterialBounces);
            const bool directional_on = cfg.directionality.enabled && on(AcousticEffect::Directionality);
            const bool delay_on = cfg.propagation.enabled && on(AcousticEffect::PropagationDelay);
            const bool reverb_on = cfg.reverb.enabled && on(AcousticEffect::Reverb);
            const bool doppler_on = cfg.doppler.mode != DopplerMode::Off && on(AcousticEffect::Doppler);

            AcousticsResult result;
            const f32 c = std::max(cfg.propagation.speed_of_sound, 1.0f);
            if (reverb_on) {
                result.reverb_rt60 = std::clamp(room.rt60 * cfg.reverb.rt60_scale, 0.0f, cfg.reverb.max_rt60);
                if (result.reverb_rt60 > 0.0f) result.reverb_rt60 = std::max(result.reverb_rt60, cfg.reverb.min_rt60);
                for (u32 b = 0; b < acoustic_bands; ++b) result.reverb_band_rt60[b] = std::clamp(room.rt60_bands[b] * cfg.reverb.rt60_scale, 0.0f, cfg.reverb.max_rt60);
                result.reverb_send = std::clamp((1.0f - room.openness) * 0.5f * cfg.reverb.send_scale * q.reverb_scale, 0.0f, 1.0f);
            }
            // Doppler is always spoken for once a provider exists, even when it is off: "off" must not fall back to the mixer's own.
            result.has_doppler = true;

            const glm::vec3 to_source = q.source - q.listener;
            const f32 distance = glm::length(to_source);
            if (distance < 1e-3f) {
                results[qi] = result;
                continue;
            }
            const glm::vec3 forward = to_source / distance;
            result.path_length = distance;
            const glm::vec3 helper = std::fabs(forward.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 u = glm::normalize(glm::cross(forward, helper));
            const glm::vec3 v = glm::cross(forward, u);

            // ---- occlusion: how much of the source can the listener "see", and what leaks through the walls ----------------------
            const u32 rays = std::max(cfg.muffling.occlusion_rays, 1u);
            std::array<f32, acoustic_bands> transmitted{0, 0, 0};
            u32 blocked = 0;
            for (u32 i = 0; i < rays; ++i) {
                glm::vec3 target = q.source;
                if (i > 0) {
                    const f32 radius = std::sqrt(static_cast<f32>(i) / static_cast<f32>(std::max(rays - 1, 1u))) * q.source_radius;
                    const f32 angle = static_cast<f32>(i) * kGolden;
                    target += (u * std::cos(angle) + v * std::sin(angle)) * radius;
                }
                u32 crossings = 0;
                const auto t = transmission_along(q.listener, target, crossings);
                for (u32 b = 0; b < acoustic_bands; ++b) transmitted[b] += t[b];
                blocked += crossings > 0 ? 1 : 0;
            }
            for (u32 b = 0; b < acoustic_bands; ++b) transmitted[b] /= static_cast<f32>(rays);
            const bool fully_hidden = blocked == rays;
            result.line_of_sight = blocked == 0;

            // ---- reflected sound: the listener's rays meet the source -----------------------------------------------------------
            std::array<f32, acoustic_bands> indirect{0, 0, 0};
            glm::vec3 arrival_sum(0.0f);
            f32 arrival_weight = 0.0f, shortest_indirect = std::numeric_limits<f32>::max();
            const auto add_path = [&](const std::array<f32, acoustic_bands> &power, f32 length, const glm::vec3 &arrival) {
                f32 mean = 0.0f;
                for (u32 b = 0; b < acoustic_bands; ++b) { indirect[b] += power[b]; mean += power[b]; }
                mean /= static_cast<f32>(acoustic_bands);
                if (mean >= cfg.directionality.min_path_power) {
                    arrival_sum += arrival * mean;
                    arrival_weight += mean;
                    shortest_indirect = std::min(shortest_indirect, length);
                }
            };
            if ((bounces_on || directional_on || delay_on) && !vertices.empty()) {
                const f32 normalise = 2.0f / static_cast<f32>(ray_count);
                std::vector<std::array<i32, 4>> seen_planes;
                for (const CachedVertex &p : vertices) {
                    const glm::vec3 to = q.source - p.position;
                    const f32 d = glm::length(to);
                    if (d < 1e-3f) continue;
                    const glm::vec3 dir = to / d;
                    const f32 facing = glm::dot(p.normal, dir);
                    const f32 total = p.length + d;
                    if (facing > 0.0f && total <= cfg.max_distance && clear_between(p.position, q.source)) {
                        // Diffuse hand-off from the surface to the source: Lambert, spread over the rays that share the room.
                        const f32 spread = (distance * distance) / (total * total) * facing * p.scattering * normalise;
                        std::array<f32, acoustic_bands> power{};
                        for (u32 b = 0; b < acoustic_bands; ++b) power[b] = p.energy[b] * spread;
                        add_path(power, total, p.first_direction);
                    }
                    // Mirror echoes: the image of the source in this wall, seen straight from the listener.
                    if (cfg.bounces.specular_paths && p.bounce == 1 && glm::dot(q.listener - p.position, p.normal) > 0.0f && facing > 0.0f) {
                        const f32 plane = glm::dot(p.position, p.normal);
                        const std::array<i32, 4> key{static_cast<i32>(std::lround(p.normal.x * 20.0f)), static_cast<i32>(std::lround(p.normal.y * 20.0f)),
                                                     static_cast<i32>(std::lround(p.normal.z * 20.0f)), static_cast<i32>(std::lround(plane * 10.0f))};
                        if (std::find(seen_planes.begin(), seen_planes.end(), key) == seen_planes.end()) {
                            seen_planes.push_back(key);
                            const glm::vec3 image = q.source - 2.0f * (glm::dot(q.source, p.normal) - plane) * p.normal;
                            const glm::vec3 image_to = image - q.listener;
                            const f32 image_length = glm::length(image_to);
                            const glm::vec3 image_dir = image_to / std::max(image_length, 1e-6f);
                            AudioRayHit wall;
                            if (image_length < cfg.max_distance && scene_->closest_hit(q.listener, image_dir, image_length, wall)) {
                                const glm::vec3 point = q.listener + image_dir * wall.distance;
                                if (std::fabs(glm::dot(point - p.position, p.normal)) < 0.05f && clear_between(point + wall.normal * 2e-3f, q.source)) {
                                    const AcousticMaterial *m = materials_ ? materials_->get(wall.material) : nullptr;
                                    const f32 mirror = m != nullptr ? 1.0f - m->scattering : 0.7f;
                                    std::array<f32, acoustic_bands> power{};
                                    for (u32 b = 0; b < acoustic_bands; ++b) {
                                        power[b] = (m != nullptr ? m->reflectivity(b) : 0.6f) * mirror * (distance * distance) / (image_length * image_length);
                                    }
                                    add_path(power, image_length, image_dir);
                                }
                            }
                        }
                    }
                }
            }
            for (u32 b = 0; b < acoustic_bands; ++b) indirect[b] = std::clamp(indirect[b] * cfg.bounces.gain, 0.0f, 1.0f);
            if (!bounces_on) indirect = {0, 0, 0};

            // ---- a way round the obstacle ------------------------------------------------------------------------------------------
            f32 detour_path = std::numeric_limits<f32>::max();
            glm::vec3 detour_point(0.0f);
            if (fully_hidden && cfg.muffling.diffraction_probes > 0 && muffling_on) {
                for (u32 k = 0; k < cfg.muffling.diffraction_probes; ++k) {
                    const f32 angle = glm::two_pi<f32>() * static_cast<f32>(k) / static_cast<f32>(cfg.muffling.diffraction_probes);
                    for (f32 radius : {1.0f, 2.5f, 5.0f}) {
                        const glm::vec3 point = q.source + (u * std::cos(angle) + v * std::sin(angle)) * radius;
                        u32 c1 = 0, c2 = 0;
                        (void)transmission_along(q.source, point, c1);
                        if (c1 != 0) continue;
                        (void)transmission_along(q.listener, point, c2);
                        if (c2 != 0) continue;
                        const f32 path = glm::length(point - q.listener) + radius;
                        if (path < detour_path) {
                            detour_path = path;
                            detour_point = point;
                        }
                    }
                }
            }
            const bool have_detour = detour_path < std::numeric_limits<f32>::max();

            // ---- muffling: what the listener hears of a sound that is not in plain view -------------------------------------------
            std::array<f32, acoustic_bands> power{};
            for (u32 b = 0; b < acoustic_bands; ++b) power[b] = transmitted[b] * transmitted[b] + indirect[b];
            if (have_detour) {
                // Diffraction bends low frequencies far better than high ones.
                const f32 ratio = std::clamp(distance / detour_path, 0.0f, 1.0f);
                const std::array<f32, acoustic_bands> bent{ratio, ratio * 0.7f, ratio * 0.35f};
                for (u32 b = 0; b < acoustic_bands; ++b) power[b] = std::max(power[b], bent[b] * bent[b]);
            }
            if (muffling_on) {
                const f32 strength = cfg.muffling.strength * q.muffling_scale;
                for (u32 b = 0; b < acoustic_bands; ++b) {
                    result.band_gain[b] = std::pow(std::sqrt(std::clamp(power[b], 0.0f, 1.0f)), strength);
                }
                const bool any_route = result.line_of_sight || have_detour || (indirect[1] + indirect[2]) * 0.5f > 1e-3f || blocked < rays;
                f32 gain = std::max(result.band_gain[1], cfg.muffling.min_gain);
                const f32 ratio = std::clamp(result.band_gain[2] / std::max(result.band_gain[1], 1e-3f), 0.0f, 1.0f);
                f32 cutoff = std::exp(lerp(std::log(cfg.muffling.min_cutoff_hz), std::log(cfg.muffling.max_cutoff_hz), ratio));
                if (!any_route) {
                    // Nothing connects the two: only what leaks through the structure, and very dull.
                    cutoff = std::min(cutoff, cfg.muffling.no_path_cutoff_hz);
                    gain *= cfg.muffling.no_path_gain;
                }
                result.broadband_gain = gain;
                result.lowpass_cutoff = std::clamp(cutoff, 20.0f, 20000.0f);
                if (reverb_on) {
                    result.reverb_send = std::clamp(result.reverb_send + (1.0f - gain) * cfg.reverb.occluded_boost * q.reverb_scale, 0.0f, 1.0f);
                }
            }

            // ---- directionality: the line of sight averaged with where the sound last bounced --------------------------------------
            glm::vec3 arrival = forward;
            if (directional_on && cfg.directionality.bounce_blend > 0.0f) {
                glm::vec3 mixed = result.line_of_sight ? forward * std::max(power[1], 0.05f) : glm::vec3(0.0f);
                if (have_detour) mixed += glm::normalize(detour_point - q.listener) * std::max(power[1], 0.05f);
                mixed += arrival_sum;
                if (glm::dot(mixed, mixed) > 1e-12f) {
                    const glm::vec3 bounce_heard = glm::normalize(mixed);
                    arrival = glm::normalize(forward * (1.0f - cfg.directionality.bounce_blend) + bounce_heard * cfg.directionality.bounce_blend);
                    if (glm::dot(arrival - forward, arrival - forward) > 1e-6f) {
                        result.apparent_direction = arrival;
                        result.has_apparent_direction = true;
                    } else {
                        arrival = forward;
                    }
                }
            }

            // ---- how long the sound takes: along the shortest route there is ----------------------------------------------------------
            f32 path = distance;
            if (!result.line_of_sight) {
                if (shortest_indirect < std::numeric_limits<f32>::max()) path = std::max(distance, shortest_indirect);
                if (have_detour) path = std::min(path > distance ? path : std::numeric_limits<f32>::max(), detour_path);
                if (path == std::numeric_limits<f32>::max()) path = distance;
            }
            result.path_length = path;
            if (delay_on) {
                const f32 scale = cfg.propagation.delay_scale * q.delay_scale;
                // With pitch Doppler the voice is resampled for motion, so the delay carries only the detour's excess:
                // a continuously changing full delay would bend the pitch a second time.
                const f32 delay = cfg.doppler.mode == DopplerMode::Pitch && doppler_on ? (path - distance) / c : path / c;
                result.extra_delay_seconds = std::clamp(delay * scale, 0.0f, cfg.propagation.max_delay_seconds);
            }

            // ---- Doppler along the path the sound really takes ---------------------------------------------------------------------------
            result.doppler = 1.0f;
            if (doppler_on && cfg.doppler.mode == DopplerMode::Pitch) {
                const glm::vec3 dir = cfg.doppler.along_arrival_path ? arrival : forward;
                const f32 factor = cfg.doppler.factor * q.doppler_scale;
                // Listener moving toward the source and source moving toward the listener both raise pitch.
                const f32 listener_along = glm::dot(q.listener_velocity, dir) * factor;
                const f32 source_along = glm::dot(q.source_velocity, dir) * factor;
                const f32 limit = c * 0.9f;
                const f32 ratio = (c + std::clamp(listener_along, -limit, limit)) / (c + std::clamp(source_along, -limit, limit));
                result.doppler = std::clamp(ratio, cfg.doppler.min_ratio, cfg.doppler.max_ratio);
            }
            results[qi] = result;
        }
    }

} // namespace SFT::Audio
