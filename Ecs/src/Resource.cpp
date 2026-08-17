#include <Ecs/src/Resource.hpp>


namespace SFT::Ecs {

    usize ResourceKeyHash::operator()(ResourceKey key) const noexcept {
        const u64 mixed = key.low ^ (key.high + 0x9e3779b97f4a7c15ull + (key.low << 6u) + (key.low >> 2u));
        return static_cast<usize>(mixed);
    }

} // namespace SFT::Ecs

