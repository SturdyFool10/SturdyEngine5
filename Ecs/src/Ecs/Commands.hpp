#pragma once

#include <Ecs/Entity.hpp>
#include <Ecs/World.hpp>

#include <Foundation/MoveOnlyFunction.hpp>

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

    class Commands;

    namespace Detail {

        using DeferredCommand = Foundation::move_only_function<void(World &) noexcept>;


        struct CommandBuffer {
            std::vector<DeferredCommand> operations;

            /// Returns the current or globally available view value.
            ///
            /// @return Returns the current view value.
            /// @note This function does not throw exceptions.
            [[nodiscard]] Commands view() noexcept;

            /// Applies the supplied operation or state to `CommandBuffer`.
            ///
            /// @param world World used or affected by the operation.
            ///
            /// @note This function does not throw exceptions.
            void apply(World &world) noexcept;
        };

    } // namespace Detail

    class Commands {
      public:
        /// Disables this construction form for `Commands`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Commands(const Commands &) = delete;
        /// Assigns a new value to this `Commands`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Commands &operator=(const Commands &) = delete;
        /// Constructs a `Commands` from another instance.
        ///
        /// @note This function does not throw exceptions.
        Commands(Commands &&) noexcept = default;
        /// Assigns a new value to this `Commands`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Commands &operator=(Commands &&) noexcept = default;

        /// Destroys or releases the `Commands` resource represented by the supplied parameters.
        ///
        /// @param entity Entity used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(Entity entity) noexcept;


        /// Spawns the supplied asynchronous work.
        ///
        /// @note This function does not throw exceptions.
        template <class... Ts>
        void spawn(Ts &&...components) noexcept {
            ZoneScopedN("Commands::spawn");
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


        /// Adds component using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        void add_component(Entity entity, T component) noexcept {
            ZoneScopedN("Commands::add_component");
            static_assert(std::is_nothrow_move_constructible_v<std::decay_t<T>>,
                          "Commands::add_component must capture the component without throwing.");
            buffer_->operations.emplace_back(
                [entity, component = std::move(component)](World &world) mutable noexcept {
                    Detail::WorldAccess::add_component(world, entity, std::move(component));
                });
        }


        /// Removes the component from its owning collection or system.
        ///
        /// @note This function does not throw exceptions.
        template <class T>
        void remove_component(Entity entity) noexcept {
            ZoneScopedN("Commands::remove_component");
            buffer_->operations.emplace_back([entity](World &world) noexcept {
                Detail::WorldAccess::remove_component<T>(world, entity);
            });
        }

        // ─── Type-erased deferred operations ─────────────────────────────────────────────────
        //
        // Siblings of the templated commands above, addressing components by id and copying their
        // bytes. They exist so a system whose body is not C++ — one registered through the C FFI —
        // can still make structural changes, which is otherwise the one thing such a system cannot
        // do safely while a schedule is running.
        //
        // Unlike the templated versions, these route through the world's validating erased API when
        // they apply. A deferred command cannot report failure to whoever queued it, and the entity
        // it names may legitimately have been destroyed by an earlier command in the same buffer, so
        // a rejected operation is dropped rather than terminating the process.

        /// Queues creation of an entity carrying the supplied components.
        ///
        /// @param component_ids Components to attach.
        /// @param component_data Initial value for each, copied immediately into the buffer so the
        ///        caller's storage need not outlive this call.
        /// @param component_sizes Byte count for each component.
        ///
        /// @note This function does not throw exceptions.
        void spawn_erased(std::span<const ComponentId> component_ids,
                          std::span<const void *const> component_data,
                          std::span<const usize> component_sizes) noexcept;

        /// Queues attaching one component to an existing entity.
        ///
        /// @param entity Entity to modify.
        /// @param component Component to attach.
        /// @param data Initial value, copied immediately into the buffer.
        /// @param size Byte count of `data`.
        ///
        /// @note This function does not throw exceptions.
        void add_component_erased(Entity entity, ComponentId component, const void *data, usize size) noexcept;

        /// Queues detaching one component from an entity.
        ///
        /// @param entity Entity to modify.
        /// @param component Component to detach.
        ///
        /// @note This function does not throw exceptions.
        void remove_component_erased(Entity entity, ComponentId component) noexcept;

      private:
        friend struct Detail::CommandBuffer;

        /// Constructs a `Commands` from the supplied initialization values.
        ///
        /// @param buffer Buffer used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Commands(Detail::CommandBuffer &buffer) noexcept;

        Detail::CommandBuffer *buffer_ = nullptr;
    };


} // namespace SFT::Ecs
