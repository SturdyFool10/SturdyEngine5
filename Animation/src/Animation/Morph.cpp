#include <Animation/Morph.hpp>

#include <glm/geometric.hpp>

#include <algorithm>

namespace SFT::Animation {

    void MorphBuilder::add_target(std::string name, std::span<const glm::vec3> position_deltas,
                                  std::span<const glm::vec3> normal_deltas, f32 default_weight, f32 epsilon) {
        Target target;
        target.name = std::move(name);
        target.default_weight = default_weight;
        const usize n = std::min(vertex_count_, position_deltas.size());
        for (usize v = 0; v < n; ++v) {
            const glm::vec3 dp = position_deltas[v];
            const glm::vec3 dn = v < normal_deltas.size() ? normal_deltas[v] : glm::vec3(0.0f);
            if (glm::any(glm::greaterThan(glm::abs(dp), glm::vec3(epsilon))) ||
                glm::any(glm::greaterThan(glm::abs(dn), glm::vec3(epsilon)))) {
                target.vertices.push_back(static_cast<u32>(v));
                target.position.push_back(dp);
                target.normal.push_back(dn);
            }
        }
        targets_.push_back(std::move(target));
    }

    MorphTargetSet MorphBuilder::build() const {
        MorphTargetSet set;
        set.vertex_offsets.assign(vertex_count_ + 1, 0);
        for (const Target &t : targets_) {
            set.target_names.push_back(t.name);
            set.default_weights.push_back(t.default_weight);
            for (u32 v : t.vertices) {
                ++set.vertex_offsets[v + 1];
            }
        }
        for (usize v = 0; v < vertex_count_; ++v) {
            set.vertex_offsets[v + 1] += set.vertex_offsets[v];
        }
        set.entries.resize(set.vertex_offsets.back());
        std::vector<u32> cursor(set.vertex_offsets.begin(), set.vertex_offsets.end() - 1);
        for (usize ti = 0; ti < targets_.size(); ++ti) {
            const Target &t = targets_[ti];
            for (usize i = 0; i < t.vertices.size(); ++i) {
                MorphEntry &e = set.entries[cursor[t.vertices[i]]++];
                e.position_delta = t.position[i];
                e.normal_delta = t.normal[i];
                e.target = static_cast<u32>(ti);
            }
        }
        return set;
    }

    void apply_morph(const MorphTargetSet &set, usize vertex, std::span<const f32> weights, glm::vec3 &position,
                     glm::vec3 &normal) {
        if (vertex + 1 >= set.vertex_offsets.size()) {
            return;
        }
        for (u32 i = set.vertex_offsets[vertex]; i < set.vertex_offsets[vertex + 1]; ++i) {
            const MorphEntry &e = set.entries[i];
            const f32 w = e.target < weights.size() ? weights[e.target] : 0.0f;
            position += e.position_delta * w;
            normal += e.normal_delta * w;
        }
    }

} // namespace SFT::Animation
