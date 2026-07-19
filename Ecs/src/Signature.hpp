#pragma once

#include <Ecs/src/Component.hpp>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace SFT::Ecs {

    // A sorted list of ComponentIds identifying one archetype. Sorted so two signatures built from
    // the same component set (regardless of the order they were named in) compare/insert/lookup
    // consistently, and so signature_is_superset() can use a linear merge instead of per-id search.
    using Signature = std::vector<ComponentId>;

    template <class... Ts>
    [[nodiscard]] Signature make_signature(ComponentRegistry &registry) {
        Signature signature{registry.component<std::remove_const_t<Ts>>()...};
        std::sort(signature.begin(), signature.end());
        const auto duplicate = std::adjacent_find(signature.begin(), signature.end());
        if (duplicate != signature.end()) {
            if (const ComponentInfo *descriptor = registry.info(*duplicate)) {
                Detail::contract_violation(
                    "ECS signature contains duplicate component '{}'.",
                    descriptor->canonical_name);
            }
            Detail::contract_violation(
                "ECS signature contains duplicate dense component ID {}.",
                *duplicate);
        }
        return signature;
    }

    // True when every id in `subset` also appears in `superset`. Both must already be sorted (every
    // Signature in this codebase always is, by construction).
    [[nodiscard]] inline bool signature_is_superset(const Signature &superset, const Signature &subset) noexcept {
        return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
    }

} // namespace SFT::Ecs
