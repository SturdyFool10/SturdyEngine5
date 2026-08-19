#include <Ecs/Archetype.hpp>


namespace SFT::Ecs {

    /// Returns the current or globally available signature value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const Signature &Archetype::signature() const noexcept { return signature_; }

    /// Returns the size for this `Ecs`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    usize Archetype::size() const noexcept { return entities_.size(); }

    /// Performs the entity at operation for `Ecs` using the supplied arguments.
    ///
    /// @param row `row` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    Entity Archetype::entity_at(u32 row) const noexcept { return entities_[row]; }

} // namespace SFT::Ecs

