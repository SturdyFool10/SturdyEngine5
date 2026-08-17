#include <Ecs/src/Signature.hpp>


namespace SFT::Ecs {

    bool signature_is_superset(const Signature &superset, const Signature &subset) noexcept {
        return std::includes(superset.begin(), superset.end(), subset.begin(), subset.end());
    }

} // namespace SFT::Ecs

