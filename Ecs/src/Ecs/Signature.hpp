#pragma once

#include <Ecs/Component.hpp>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace SFT::Ecs {


    using Signature = std::vector<ComponentId>;

    /// Creates a signature value from the supplied arguments.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


    /// Reports whether signature is superset.
    ///
    /// @param superset `superset` value used by the operation.
    /// @param subset `subset` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool signature_is_superset(const Signature &superset, const Signature &subset) noexcept;

} // namespace SFT::Ecs
