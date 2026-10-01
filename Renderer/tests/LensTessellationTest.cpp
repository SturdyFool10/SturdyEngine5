/// lens_tessellate() splits over-long triangle edges so the vertex-warp camera lens does not bend large triangles.
/// It must bound edge length, keep the surface area, and stay watertight (no T-junctions) across shared edges.

#include <Renderer/LensTessellation.hpp>

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <iostream>
#include <map>
#include <utility>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    using SFT::Renderer::GeometryVertex;

    GeometryVertex vertex(float x, float y, float z) {
        GeometryVertex v;
        v.position = {x, y, z};
        v.normal = {0.0f, 0.0f, 1.0f};
        v.uv = {x, y};
        return v;
    }

    float area(const SFT::Renderer::LensTessellatedMesh &mesh) {
        float total = 0.0f;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const glm::vec3 a = mesh.vertices[mesh.indices[t]].position;
            const glm::vec3 b = mesh.vertices[mesh.indices[t + 1]].position;
            const glm::vec3 c = mesh.vertices[mesh.indices[t + 2]].position;
            total += 0.5f * glm::length(glm::cross(b - a, c - a));
        }
        return total;
    }

    float longest_edge(const SFT::Renderer::LensTessellatedMesh &mesh) {
        float longest = 0.0f;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                const glm::vec3 a = mesh.vertices[mesh.indices[t + e]].position;
                const glm::vec3 b = mesh.vertices[mesh.indices[t + (e + 1) % 3]].position;
                longest = std::max(longest, glm::length(a - b));
            }
        }
        return longest;
    }
} // namespace

int main() {
    using SFT::Renderer::lens_tessellate;

    // A quad made of two triangles sharing the long diagonal: (0,0)-(4,0)-(4,4)-(0,4).
    const std::vector<GeometryVertex> quad = {vertex(0, 0, 0), vertex(4, 0, 0), vertex(4, 4, 0), vertex(0, 4, 0)};
    const std::vector<uint32_t> quad_indices = {0, 1, 2, 0, 2, 3};

    const auto untouched = lens_tessellate(quad, quad_indices, 100.0f);
    check(!untouched.subdivided, "nothing to split when every edge is short enough");
    check(untouched.indices == quad_indices, "an unsplit mesh is returned unchanged");

    const auto split = lens_tessellate(quad, quad_indices, 1.0f);
    check(split.subdivided, "long edges are split");
    check(longest_edge(split) <= 1.0f + 1.0e-4f, "no edge stays longer than the limit");
    check(std::abs(area(split) - 16.0f) < 1.0e-3f, "the surface area is preserved");

    // Watertight: every undirected edge is used by at most two triangles, and an edge used twice is used in
    // opposite directions (consistent winding). A T-junction would leave a long edge next to two short ones.
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
    for (size_t t = 0; t + 2 < split.indices.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = split.indices[t + e];
            const uint32_t b = split.indices[t + (e + 1) % 3];
            ++directed[{a, b}];
        }
    }
    bool watertight = true;
    for (const auto &[edge, count] : directed) {
        if (count != 1) watertight = false;                        // each directed edge exactly once
        const auto reverse = directed.find({edge.second, edge.first});
        const bool boundary = reverse == directed.end();
        const glm::vec3 a = split.vertices[edge.first].position;
        const glm::vec3 b = split.vertices[edge.second].position;
        const bool on_border = (a.x == b.x && (a.x == 0.0f || a.x == 4.0f)) || (a.y == b.y && (a.y == 0.0f || a.y == 4.0f));
        if (boundary && !on_border) watertight = false;            // an interior edge must have a partner
    }
    check(watertight, "the split mesh has no T-junctions or duplicated edges");

    const auto capped = lens_tessellate(quad, quad_indices, 0.01f, 50);
    check(capped.indices.size() / 3 <= 50, "the triangle budget is respected (last complete pass kept)");
    return failures == 0 ? 0 : 1;
}
