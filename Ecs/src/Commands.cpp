#include <Ecs/src/Commands.hpp>


namespace SFT::Ecs::Detail {

    void CommandBuffer::apply(World &world) noexcept {
        ZoneScopedN("CommandBuffer::apply");
        for (DeferredCommand &operation : operations) {
            operation(world);
        }
        operations.clear();
    }

} // namespace SFT::Ecs::Detail

namespace SFT::Ecs {

    void Commands::destroy(Entity entity) noexcept {
        ZoneScopedN("Commands::destroy");
        buffer_->operations.emplace_back([entity](World &world) noexcept {
            Detail::WorldAccess::destroy(world, entity);
        });
    }

    Commands Detail::CommandBuffer::view() noexcept {
        return Commands{*this};
    }

} // namespace SFT::Ecs


namespace SFT::Ecs {

    Commands::Commands(Detail::CommandBuffer &buffer) noexcept : buffer_(&buffer) {}

} // namespace SFT::Ecs

