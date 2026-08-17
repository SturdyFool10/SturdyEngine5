#include <Ecs/src/Archetype.hpp>


namespace SFT::Ecs {

    const Signature &Archetype::signature() const noexcept { return signature_; }

    usize Archetype::size() const noexcept { return entities_.size(); }

    Entity Archetype::entity_at(u32 row) const noexcept { return entities_[row]; }

} // namespace SFT::Ecs

