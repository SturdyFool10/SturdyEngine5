#include <Ecs/Signature.hpp>


namespace SFT::Ecs {

    /// Reports whether signature is superset.
    ///
    /// @param superset `superset` value used by the operation.
    /// @param subset `subset` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool signature_is_superset(const Signature &superset, const Signature &subset) noexcept {
        return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
    }

} // namespace SFT::Ecs

