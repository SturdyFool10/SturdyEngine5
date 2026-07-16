#pragma once

#include <Ecs/Component.hpp>

#include <algorithm>
#include <vector>

namespace SFT::Ecs {

    // A sorted list of ComponentIds identifying one archetype. Sorted so two signatures built from
    // the same component set (regardless of the order they were named in) compare/insert/lookup
    // consistently, and so signature_is_superset() can use a linear merge instead of per-id search.
    using Signature = std::vector<ComponentId>;

    template <class... Ts>
    [[nodiscard]] Signature make_signature() {
        Signature signature{component_id<Ts>()...};
        std::sort(signature.begin(), signature.end());
        return signature;
    }

    // True when every id in `subset` also appears in `superset`. Both must already be sorted (every
    // Signature in this codebase always is, by construction).
    [[nodiscard]] inline bool signature_is_superset(const Signature &superset, const Signature &subset) noexcept {
        return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
    }

} // namespace SFT::Ecs
