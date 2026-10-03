#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec3.hpp>

#include <span>
#include <string>
#include <vector>

namespace SFT::Animation {

    /// One vertex's offset for one morph target (blend shape).
    struct MorphEntry {
        glm::vec3 position_delta{0.0f};
        u32 target = 0;
        glm::vec3 normal_delta{0.0f};
        u32 padding = 0;
    };
    static_assert(sizeof(MorphEntry) == 32);

    /// Sparse morph targets for one mesh primitive, stored per vertex (CSR): the entries of vertex `v` are
    /// `entries[vertex_offsets[v] .. vertex_offsets[v + 1])`. Only vertices a target actually moves are stored, so
    /// facial rigs with dozens of local shapes stay small. This is also the layout the GPU skinning pass reads.
    struct MorphTargetSet {
        std::vector<UString> target_names;
        std::vector<f32> default_weights;
        std::vector<u32> vertex_offsets;
        std::vector<MorphEntry> entries;

        [[nodiscard]] u32 target_count() const noexcept { return static_cast<u32>(target_names.size()); }
        [[nodiscard]] usize vertex_count() const noexcept {
            return vertex_offsets.empty() ? 0 : vertex_offsets.size() - 1;
        }
        [[nodiscard]] bool empty() const noexcept { return target_names.empty(); }
    };

    /// Builds a `MorphTargetSet` from per-target dense delta arrays (what every importer starts from).
    class MorphBuilder {
      public:
        explicit MorphBuilder(usize vertex_count) : vertex_count_(vertex_count) {}

        /// `normal_deltas` may be empty. Deltas below `epsilon` in every component are dropped.
        void add_target(UString name, std::span<const glm::vec3> position_deltas,
                        std::span<const glm::vec3> normal_deltas = {}, f32 default_weight = 0.0f,
                        f32 epsilon = 1e-7f);

        [[nodiscard]] MorphTargetSet build() const;

      private:
        struct Target {
            UString name;
            f32 default_weight = 0.0f;
            std::vector<u32> vertices;
            std::vector<glm::vec3> position;
            std::vector<glm::vec3> normal;
        };
        usize vertex_count_ = 0;
        std::vector<Target> targets_;
    };

    /// Reference CPU evaluation: position/normal of vertex `vertex` after applying `weights`.
    void apply_morph(const MorphTargetSet &set, usize vertex, std::span<const f32> weights, glm::vec3 &position,
                     glm::vec3 &normal);

} // namespace SFT::Animation
