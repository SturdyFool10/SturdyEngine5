#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/Displacement/DisplacementTypes.hpp>

using std::span;
using std::vector;

// Draw binning by displacement shader variant (plans/displacement-system.md section 7, item 3).
//
// The per-pixel algorithm is a compile-time shader choice, but select_algorithm_for_view picks a
// (possibly different) rung per object per frame. Submitted in scene order, neighbouring draws would
// alternate between, say, ParallaxOcclusion and HierarchicalCellExact pipelines: pipeline thrash, and
// — more importantly on the GPU — a wave that straddles two draws' pixels never mixes their
// traversal costs *within* one draw, but interleaved draws of very different cost defeat overlap of
// cheap and expensive work. Grouping draws by variant makes each run of draws share one pipeline.
//
// This is a pure CPU utility: the Renderer owns building the key (variant_key) and consuming the
// permutation (draw order + pipeline binds). It knows nothing about the RHI.
namespace SFT::Renderer::Displacement {

    /// Everything that changes which compiled shader variant a displaced draw needs. Ordered so that
    /// packing it (variant_key) sorts bins from cheapest to most expensive algorithm first.
    struct DisplacementVariant {
        /// The algorithm actually used *this frame* (select_algorithm_for_view), not plan.algorithm.
        HeightfieldAlgorithm algorithm = HeightfieldAlgorithm::NormalOnly;
        GeometryPath geometry = GeometryPath::None;
        DepthPolicy depth = DepthPolicy::BaseSurface;
        b8 self_shadow = false;
        b8 corner_gather = false;
        b8 start_level_from_footprint = false;

        [[nodiscard]] friend bool operator==(const DisplacementVariant &a, const DisplacementVariant &b) noexcept {
            return a.algorithm == b.algorithm && a.geometry == b.geometry && a.depth == b.depth &&
                   static_cast<bool>(a.self_shadow) == static_cast<bool>(b.self_shadow) &&
                   static_cast<bool>(a.corner_gather) == static_cast<bool>(b.corner_gather) &&
                   static_cast<bool>(a.start_level_from_footprint) == static_cast<bool>(b.start_level_from_footprint);
        }
    };

    /// Variant a plan resolves to when the given per-view algorithm is used for one object. The
    /// algorithm is passed separately because it changes per frame while the plan does not.
    [[nodiscard]] DisplacementVariant variant_for(const DisplacementPlan &plan,
                                                  HeightfieldAlgorithm per_view_algorithm) noexcept;

    /// Stable 32-bit key: equal variants get equal keys, distinct variants distinct keys, and a numerically
    /// smaller key never has a more expensive algorithm than a larger one (algorithm is the high field).
    [[nodiscard]] u32 variant_key(const DisplacementVariant &variant) noexcept;

    struct VariantBin {
        u32 key = 0;
        /// Range [first, first + count) into BinnedDraws::order.
        u32 first = 0;
        u32 count = 0;
    };

    struct BinnedDraws {
        /// Input draw indices, grouped by key. Within a bin the original relative order is preserved (stable),
        /// so a caller's earlier front-to-back sort survives.
        vector<u32> order;
        /// One entry per distinct key, ascending by key.
        vector<VariantBin> bins;
    };

    /// Groups draws by key. `keys[i]` is draw i's variant_key.
    [[nodiscard]] BinnedDraws bin_by_variant(span<const u32> keys);

} // namespace SFT::Renderer::Displacement
