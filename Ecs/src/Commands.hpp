#pragma once

#include <Ecs/src/Entity.hpp>
#include <Ecs/src/World.hpp>

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    class Commands;

    namespace Detail {

        using DeferredCommand = std::move_only_function<void(World &) noexcept>;

        // One async query chunk owns one command buffer, so recording requires no locks. Schedule
        // applies buffers in dispatch order after every task in the current dependency stage has
        // completed, keeping structural mutation deterministic and query storage stable.
        struct CommandBuffer {
            std::vector<DeferredCommand> operations;

            [[nodiscard]] Commands view() noexcept;

            void apply(World &world) noexcept {
                for (DeferredCommand &operation : operations) {
                    operation(world);
                }
                operations.clear();
            }
        };

    } // namespace Detail

    class Commands {
      public:
        Commands(const Commands &) = delete;
        Commands &operator=(const Commands &) = delete;
        Commands(Commands &&) noexcept = default;
        Commands &operator=(Commands &&) noexcept = default;

        void destroy(Entity entity) noexcept {
            buffer_->operations.emplace_back([entity](World &world) noexcept {
                Detail::WorldAccess::destroy(world, entity);
            });
        }

        // Spawning is deferred, so no Entity can be returned synchronously. The components are
        // owned by the command until the stage boundary and moved into the new archetype row there.
        template <class... Ts>
        void spawn(Ts &&...components) noexcept {
            static_assert(sizeof...(Ts) > 0, "Commands::spawn requires at least one component.");
            static_assert((std::is_nothrow_constructible_v<std::decay_t<Ts>, Ts &&> && ...),
                          "Commands::spawn must capture each component without throwing.");
            static_assert((std::is_nothrow_move_constructible_v<std::decay_t<Ts>> && ...),
                          "Deferred spawn components must be nothrow move-constructible.");

            auto owned_components = std::tuple<std::decay_t<Ts>...>{std::forward<Ts>(components)...};
            buffer_->operations.emplace_back(
                [owned_components = std::move(owned_components)](World &world) mutable noexcept {
                    std::apply(
                        [&world](auto &...values) noexcept {
                            (void)Detail::WorldAccess::spawn(world, std::move(values)...);
                        },
                        owned_components);
                });
        }

        // Deferred archetype transition: T is moved into the command buffer now, then placement-
        // constructed into `entity`'s new archetype row at the stage boundary. Contract violation
        // (at apply time) if `entity` is dead or already has T by then.
        template <class T>
        void add_component(Entity entity, T component) noexcept {
            static_assert(std::is_nothrow_move_constructible_v<std::decay_t<T>>,
                          "Commands::add_component must capture the component without throwing.");
            buffer_->operations.emplace_back(
                [entity, component = std::move(component)](World &world) mutable noexcept {
                    Detail::WorldAccess::add_component(world, entity, std::move(component));
                });
        }

        // Deferred archetype transition: destroys T on `entity` at the stage boundary. Contract
        // violation (at apply time) if `entity` is dead or doesn't have T by then.
        template <class T>
        void remove_component(Entity entity) noexcept {
            buffer_->operations.emplace_back([entity](World &world) noexcept {
                Detail::WorldAccess::remove_component<T>(world, entity);
            });
        }

      private:
        friend struct Detail::CommandBuffer;

        explicit Commands(Detail::CommandBuffer &buffer) noexcept : buffer_(&buffer) {}

        Detail::CommandBuffer *buffer_ = nullptr;
    };

    inline Commands Detail::CommandBuffer::view() noexcept {
        return Commands{*this};
    }

} // namespace SFT::Ecs
