#include <Renderer/LensTessellation.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <glm/geometric.hpp>

namespace SFT::Renderer {

    namespace {

        constexpr u32 kNoVertex = ~0u;

        [[nodiscard]] u64 edge_key(u32 a, u32 b) noexcept {
            if (a > b) std::swap(a, b);
            return (static_cast<u64>(a) << 32) | b;
        }

        [[nodiscard]] GeometryVertex midpoint(const GeometryVertex &a, const GeometryVertex &b) {
            GeometryVertex result;
            result.position = (a.position + b.position) * 0.5f;
            const glm::vec3 normal = a.normal + b.normal;
            result.normal = glm::dot(normal, normal) > 1.0e-12f ? glm::normalize(normal) : a.normal;
            result.uv = (a.uv + b.uv) * 0.5f;
            result.color = (a.color + b.color) * 0.5f;
            const glm::vec3 tangent = glm::vec3{a.tangent} + glm::vec3{b.tangent};
            result.tangent = glm::dot(tangent, tangent) > 1.0e-12f ? glm::vec4{glm::normalize(tangent), a.tangent.w} : a.tangent;
            return result;
        }

    } // namespace

    LensTessellatedMesh lens_tessellate(std::span<const GeometryVertex> vertices, std::span<const u32> indices,
                                        f32 max_edge_length, usize max_triangles) {
        LensTessellatedMesh result;
        result.vertices.assign(vertices.begin(), vertices.end());
        result.indices.assign(indices.begin(), indices.end());
        if (indices.size() < 3 || !(max_edge_length > 0.0f)) {
            return result;
        }
        const f32 max_edge_squared = max_edge_length * max_edge_length;

        constexpr int kMaxPasses = 12;
        for (int pass = 0; pass < kMaxPasses; ++pass) {
            std::unordered_map<u64, u32> midpoints;
            std::vector<GeometryVertex> pass_vertices = result.vertices;
            const auto edge_too_long = [&](u32 a, u32 b) {
                const glm::vec3 d = pass_vertices[a].position - pass_vertices[b].position;
                return glm::dot(d, d) > max_edge_squared;
            };
            // Only edges that are actually too long get a midpoint (decided per edge, so both triangles sharing it agree).
            const auto split_of = [&](u32 a, u32 b) -> u32 {
                if (!edge_too_long(a, b)) return kNoVertex;
                const u64 key = edge_key(a, b);
                if (const auto found = midpoints.find(key); found != midpoints.end()) return found->second;
                const u32 index = static_cast<u32>(pass_vertices.size());
                const u32 low = std::min(a, b);
                const u32 high = std::max(a, b);
                pass_vertices.push_back(midpoint(pass_vertices[low], pass_vertices[high]));
                midpoints.emplace(key, index);
                return index;
            };

            std::vector<u32> next;
            next.reserve(result.indices.size());
            bool any_split = false;
            for (usize t = 0; t + 2 < result.indices.size(); t += 3) {
                const u32 i[3] = {result.indices[t], result.indices[t + 1], result.indices[t + 2]};
                const u32 m[3] = {split_of(i[0], i[1]), split_of(i[1], i[2]), split_of(i[2], i[0])};
                const int split_count = (m[0] != kNoVertex) + (m[1] != kNoVertex) + (m[2] != kNoVertex);
                const auto emit = [&](u32 a, u32 b, u32 c) {
                    next.push_back(a);
                    next.push_back(b);
                    next.push_back(c);
                };
                if (split_count == 0) {
                    emit(i[0], i[1], i[2]);
                    continue;
                }
                any_split = true;
                if (split_count == 3) {
                    emit(i[0], m[0], m[2]);
                    emit(m[0], i[1], m[1]);
                    emit(m[2], m[1], i[2]);
                    emit(m[0], m[1], m[2]);
                } else if (split_count == 1) {
                    const int r = m[0] != kNoVertex ? 0 : (m[1] != kNoVertex ? 1 : 2);
                    const u32 a = i[r], b = i[(r + 1) % 3], c = i[(r + 2) % 3];
                    emit(a, m[r], c);
                    emit(m[r], b, c);
                } else {
                    const int r = m[0] == kNoVertex ? 0 : (m[1] == kNoVertex ? 1 : 2); // the unsplit edge
                    const u32 a = i[(r + 1) % 3], b = i[(r + 2) % 3], c = i[r];
                    const u32 mab = m[(r + 1) % 3], mbc = m[(r + 2) % 3];
                    emit(mab, b, mbc);
                    const glm::vec3 d0 = pass_vertices[a].position - pass_vertices[mbc].position;
                    const glm::vec3 d1 = pass_vertices[mab].position - pass_vertices[c].position;
                    if (glm::dot(d0, d0) < glm::dot(d1, d1)) {
                        emit(a, mab, mbc);
                        emit(a, mbc, c);
                    } else {
                        emit(a, mab, c);
                        emit(mab, mbc, c);
                    }
                }
            }
            if (!any_split) {
                break;
            }
            if (next.size() / 3 > max_triangles) {
                break; // keep the last complete (conforming) pass
            }
            result.vertices = std::move(pass_vertices);
            result.indices = std::move(next);
            result.subdivided = true;
        }
        return result;
    }

} // namespace SFT::Renderer
