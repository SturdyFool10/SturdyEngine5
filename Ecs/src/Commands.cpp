#include <Ecs/src/Commands.hpp>


namespace SFT::Ecs::Detail {

    /// Applies the supplied operation or state to `Detail`.
    ///
    /// @param world World used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void CommandBuffer::apply(World &world) noexcept {
        ZoneScopedN("CommandBuffer::apply");
        for (DeferredCommand &operation : operations) {
            operation(world);
        }
        operations.clear();
    }

} // namespace SFT::Ecs::Detail

namespace SFT::Ecs {

    /// Destroys or releases the `Ecs` resource represented by the supplied parameters.
    ///
    /// @param entity Entity used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Commands::destroy(Entity entity) noexcept {
        ZoneScopedN("Commands::destroy");
        buffer_->operations.emplace_back([entity](World &world) noexcept {
            Detail::WorldAccess::destroy(world, entity);
        });
    }

    /// Returns the current or globally available view value.
    ///
    /// @return Returns the current view value.
    /// @note This function does not throw exceptions.
    Commands Detail::CommandBuffer::view() noexcept {
        return Commands{*this};
    }

} // namespace SFT::Ecs


namespace SFT::Ecs {

    /// Performs the commands operation for `Ecs` using the supplied arguments.
    ///
    /// @param buffer Buffer used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    Commands::Commands(Detail::CommandBuffer &buffer) noexcept : buffer_(&buffer) {}

} // namespace SFT::Ecs

