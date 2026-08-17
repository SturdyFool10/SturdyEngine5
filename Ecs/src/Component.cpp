#include <Ecs/src/Component.hpp>


namespace SFT::Ecs {

    usize ComponentKeyHash::operator()(ComponentKey key) const noexcept {
        const u64 mixed = key.low ^ (key.high + 0x9e3779b97f4a7c15ull + (key.low << 6u) + (key.low >> 2u));
        return static_cast<usize>(mixed);
    }

} // namespace SFT::Ecs

