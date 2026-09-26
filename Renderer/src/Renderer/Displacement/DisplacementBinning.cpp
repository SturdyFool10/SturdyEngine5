#include <Renderer/Displacement/DisplacementBinning.hpp>

#include <algorithm>
#include <numeric>

namespace SFT::Renderer::Displacement {

    DisplacementVariant variant_for(const DisplacementPlan &plan, HeightfieldAlgorithm per_view_algorithm) noexcept {
        DisplacementVariant v;
        v.algorithm = per_view_algorithm;
        v.geometry = plan.geometry;
        v.depth = plan.depth;
        v.self_shadow = plan.self_shadow != SelfShadowQuality::Off;
        // The optimizations only exist inside the hierarchical/cell-exact traversal; a cheaper rung has no
        // gather or start level, so it must not split a bin over a define it does not read.
        const bool traces = per_view_algorithm >= HeightfieldAlgorithm::CellExact;
        v.corner_gather = plan.corner_gather && traces;
        v.start_level_from_footprint =
            plan.start_level_from_footprint && per_view_algorithm == HeightfieldAlgorithm::HierarchicalCellExact;
        return v;
    }

    u32 variant_key(const DisplacementVariant &v) noexcept {
        // [algorithm:8][geometry:8][depth:4][flags:4]
        u32 key = static_cast<u32>(v.algorithm) << 24;
        key |= static_cast<u32>(v.geometry) << 16;
        key |= static_cast<u32>(v.depth) << 8;
        key |= (v.self_shadow ? 1u : 0u) | (v.corner_gather ? 2u : 0u) | (v.start_level_from_footprint ? 4u : 0u);
        return key;
    }

    BinnedDraws bin_by_variant(span<const u32> keys) {
        BinnedDraws out;
        out.order.resize(keys.size());
        std::iota(out.order.begin(), out.order.end(), 0u);
        std::stable_sort(out.order.begin(), out.order.end(), [&](u32 a, u32 b) { return keys[a] < keys[b]; });

        for (u32 i = 0; i < out.order.size();) {
            const u32 key = keys[out.order[i]];
            u32 j = i;
            while (j < out.order.size() && keys[out.order[j]] == key) {
                ++j;
            }
            out.bins.push_back(VariantBin{key, i, j - i});
            i = j;
        }
        return out;
    }

} // namespace SFT::Renderer::Displacement
