#pragma once

#include <Async/Scheduler.hpp>
#include <Async/Task.hpp>

#include <Ecs/Commands.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Query.hpp>
#include <Ecs/Resource.hpp>
#include <Ecs/World.hpp>

#include <concepts>
#include <deque>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // Derived from a safe per-entity system's component-reference parameters. Stable keys make the
    // declaration independent of one World's dense registry IDs.
    struct SystemAccess {
        std::vector<ComponentKey> reads;
        std::vector<ComponentKey> writes;
        std::vector<ResourceKey> resource_reads;
        std::vector<ResourceKey> resource_writes;
    };

    // Two systems conflict whenever either writes a component touched by the other. Read/read and
    // access to entirely different component columns are safe to execute concurrently.
    template <class Key>
    [[nodiscard]] inline bool access_sets_conflict(const std::vector<Key> &a_reads,
                                                   const std::vector<Key> &a_writes,
                                                   const std::vector<Key> &b_reads,
                                                   const std::vector<Key> &b_writes) noexcept {
        for (Key write : a_writes) {
            for (Key read : b_reads) {
                if (read == write) {
                    return true;
                }
            }
            for (Key other_write : b_writes) {
                if (other_write == write) {
                    return true;
                }
            }
        }
        for (Key write : b_writes) {
            for (Key read : a_reads) {
                if (read == write) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] inline bool system_access_conflicts(const SystemAccess &a, const SystemAccess &b) noexcept {
        return access_sets_conflict(a.reads, a.writes, b.reads, b.writes) ||
               access_sets_conflict(a.resource_reads, a.resource_writes, b.resource_reads, b.resource_writes);
    }

    struct ScheduleConfig {
        // Small queries remain one task. Larger archetypes are divided toward
        // worker_count * tasks_per_worker without creating tiny scheduling-granularity tasks.
        usize minimum_rows_per_task = 128;
        usize tasks_per_worker = 2;
    };

    namespace Detail {

        template <class R, class... Args>
        struct CallableSignature {
            using ArgsTuple = std::tuple<Args...>;
            using Return = R;
        };

        template <class F>
        struct CallableTraits : CallableTraits<decltype(&F::operator())> {};

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...) const> : CallableSignature<R, Args...> {};

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...) const noexcept> : CallableSignature<R, Args...> {};

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...)> : CallableSignature<R, Args...> {};

        template <class C, class R, class... Args>
        struct CallableTraits<R (C::*)(Args...) noexcept> : CallableSignature<R, Args...> {};

        template <class R, class... Args>
        struct CallableTraits<R (*)(Args...)> : CallableSignature<R, Args...> {};

        template <class R, class... Args>
        struct CallableTraits<R (*)(Args...) noexcept> : CallableSignature<R, Args...> {};

        template <class Q>
        struct QueryAccessOf;

        template <class... Ts>
        struct QueryAccessOf<Query<Ts...>> {
            [[nodiscard]] static SystemAccess access() {
                SystemAccess result;
                (accumulate<Ts>(result), ...);
                return result;
            }

          private:
            template <class T>
            static void accumulate(SystemAccess &result) {
                if constexpr (std::is_const_v<T>) {
                    result.reads.push_back(component_key<std::remove_const_t<T>>());
                } else {
                    result.writes.push_back(component_key<T>());
                }
            }
        };

        template <class ResourceTuple>
        struct ResourceAccessOf;

        template <class... ResourceArgs>
        struct ResourceAccessOf<std::tuple<ResourceArgs...>> {
            [[nodiscard]] static constexpr bool has_writes() noexcept {
                return (ResourceArgumentTraits<ResourceArgs>::IsWrite || ... || false);
            }

            static void accumulate(SystemAccess &result) {
                (accumulate_one<ResourceArgs>(result), ...);
            }

          private:
            template <class Argument>
            static void accumulate_one(SystemAccess &result) {
                using Traits = ResourceArgumentTraits<Argument>;
                using Resource = typename Traits::Resource;
                if constexpr (Traits::IsWrite) {
                    result.resource_writes.push_back(resource_key<Resource>());
                } else {
                    result.resource_reads.push_back(resource_key<Resource>());
                }
            }
        };

        template <class ArgsTuple, class Indices>
        struct QueryFromSystemArguments;

        template <class ArgsTuple, usize... Is>
        struct QueryFromSystemArguments<ArgsTuple, std::index_sequence<Is...>> {
            // Argument zero is Entity. The component parameters follow and are lvalue references;
            // removing only the reference preserves const as the read/write declaration.
            using Type = Query<std::remove_reference_t<std::tuple_element_t<Is + 1, ArgsTuple>>...>;
        };

        template <class ArgsTuple, usize Offset, class Indices>
        struct TupleSlice;

        template <class ArgsTuple, usize Offset, usize... Is>
        struct TupleSlice<ArgsTuple, Offset, std::index_sequence<Is...>> {
            using Type = std::tuple<std::tuple_element_t<Offset + Is, ArgsTuple>...>;
        };

        template <class ArgsTuple>
        struct EntitySystemTraits {
            static constexpr usize ArgumentCount = std::tuple_size_v<ArgsTuple>;
            static_assert(ArgumentCount >= 2,
                          "An ECS system must accept Entity followed by at least one component reference.");

            using EntityArgument = std::tuple_element_t<0, ArgsTuple>;
            using LastArgument = std::tuple_element_t<ArgumentCount - 1, ArgsTuple>;
            static_assert(std::same_as<EntityArgument, Entity>,
                          "A safe ECS system's first parameter must be Entity by value.");

            static constexpr bool HasCommands = std::same_as<std::remove_cvref_t<LastArgument>, Commands>;
            static_assert(!HasCommands || std::same_as<LastArgument, Commands &>,
                          "Commands must be the final system parameter and must be passed as Commands&.");

            static constexpr usize PayloadEnd = ArgumentCount - static_cast<usize>(HasCommands);

            template <usize Index>
            [[nodiscard]] static consteval usize component_prefix_count() {
                if constexpr (Index >= PayloadEnd) {
                    return 0;
                } else if constexpr (is_resource_argument_v<std::tuple_element_t<Index, ArgsTuple>>) {
                    return 0;
                } else {
                    return 1 + component_prefix_count<Index + 1>();
                }
            }

            static constexpr usize ComponentCount = component_prefix_count<1>();
            static_assert(ComponentCount > 0, "An ECS system must declare at least one component reference.");
            static constexpr usize ResourceCount = PayloadEnd - 1 - ComponentCount;
            using ComponentIndices = std::make_index_sequence<ComponentCount>;
            using ResourceIndices = std::make_index_sequence<ResourceCount>;

            template <usize... Is>
            [[nodiscard]] static consteval bool valid_component_arguments(std::index_sequence<Is...>) {
                return ((std::is_lvalue_reference_v<std::tuple_element_t<Is + 1, ArgsTuple>> &&
                         !std::is_volatile_v<std::remove_reference_t<std::tuple_element_t<Is + 1, ArgsTuple>>> &&
                         !std::same_as<std::remove_cvref_t<std::tuple_element_t<Is + 1, ArgsTuple>>, Commands>) &&
                        ...);
            }

            template <usize... Is>
            [[nodiscard]] static consteval bool valid_resource_arguments(std::index_sequence<Is...>) {
                return ((is_resource_argument_v<std::tuple_element_t<ComponentCount + 1 + Is, ArgsTuple>> &&
                         std::same_as<std::tuple_element_t<ComponentCount + 1 + Is, ArgsTuple>,
                                      std::remove_cvref_t<std::tuple_element_t<ComponentCount + 1 + Is, ArgsTuple>>>) &&
                        ...);
            }

            static_assert(valid_component_arguments(ComponentIndices{}),
                          "ECS component parameters must be non-volatile lvalue references; const T& reads and T& writes.");
            static_assert(valid_resource_arguments(ResourceIndices{}),
                          "ECS resources must follow all component parameters and be passed by value as ReadResource<T> or WriteResource<T>.");

            using QueryType = typename QueryFromSystemArguments<ArgsTuple, ComponentIndices>::Type;
            using ResourceArguments = typename TupleSlice<ArgsTuple, ComponentCount + 1, ResourceIndices>::Type;
        };

        using AsyncTaskList = std::vector<Async::TaskHandle<void>>;
        using CommandBufferList = std::deque<CommandBuffer>;
        using SystemDispatch = std::function<void(World &, usize, usize, AsyncTaskList &, CommandBufferList &)>;

        template <class QueryType, class ResourceTuple, bool HasCommands>
        struct EntitySystemRunner;

        template <class... Ts, class... ResourceArgs, bool HasCommands>
        struct EntitySystemRunner<Query<Ts...>, std::tuple<ResourceArgs...>, HasCommands> {
            template <class F>
            [[nodiscard]] static consteval bool valid_callback() {
                if constexpr (HasCommands) {
                    if constexpr (std::is_nothrow_invocable_v<const F &, Entity, Ts &..., ResourceArgs..., Commands &>) {
                        return std::same_as<std::invoke_result_t<const F &, Entity, Ts &..., ResourceArgs..., Commands &>, void>;
                    } else {
                        return false;
                    }
                } else {
                    if constexpr (std::is_nothrow_invocable_v<const F &, Entity, Ts &..., ResourceArgs...>) {
                        return std::same_as<std::invoke_result_t<const F &, Entity, Ts &..., ResourceArgs...>, void>;
                    } else {
                        return false;
                    }
                }
            }

            template <class Argument>
            [[nodiscard]] static Argument resolve_resource(World &world) noexcept {
                using Traits = ResourceArgumentTraits<Argument>;
                using Resource = typename Traits::Resource;
                if constexpr (Traits::IsWrite) {
                    return ResourceViewFactory::write(WorldAccess::resource<Resource>(world));
                } else {
                    return ResourceViewFactory::read(std::as_const(WorldAccess::resource<Resource>(world)));
                }
            }

            [[nodiscard]] static std::tuple<ResourceArgs...> resolve_resources(World &world) noexcept {
                return std::tuple<ResourceArgs...>{resolve_resource<ResourceArgs>(world)...};
            }

            template <class F>
            static void invoke_without_commands(const F &system,
                                                std::tuple<ResourceArgs...> &resources,
                                                Entity entity,
                                                Ts &...components) noexcept {
                std::apply(
                    [&](ResourceArgs... resource_views) noexcept {
                        std::invoke(system, entity, components..., resource_views...);
                    },
                    resources);
            }

            template <class F>
            static void invoke_with_commands(const F &system,
                                             std::tuple<ResourceArgs...> &resources,
                                             Commands &commands,
                                             Entity entity,
                                             Ts &...components) noexcept {
                std::apply(
                    [&](ResourceArgs... resource_views) noexcept {
                        std::invoke(system, entity, components..., resource_views..., commands);
                    },
                    resources);
            }

            template <class F>
            [[nodiscard]] static SystemDispatch make_dispatch(F fn) {
                return [fn = std::move(fn)](World &world,
                                            usize minimum_rows_per_task,
                                            usize target_parallelism,
                                            AsyncTaskList &tasks,
                                            CommandBufferList &command_buffers) mutable {
                    auto query = WorldAccess::query<Ts...>(world);
                    auto chunks = query.chunks(minimum_rows_per_task, target_parallelism);
                    auto resources = resolve_resources(world);

                    // A mutable singleton is one memory location, so a system writing any resource
                    // processes all its chunks in one Async task. This preserves automatic safety;
                    // sharded/deferred resources can add an explicitly parallel reduction path later.
                    if constexpr (ResourceAccessOf<std::tuple<ResourceArgs...>>::has_writes()) {
                        if (chunks.empty()) {
                            return;
                        }
                        CommandBuffer *command_buffer = nullptr;
                        if constexpr (HasCommands) {
                            command_buffers.emplace_back();
                            command_buffer = &command_buffers.back();
                        }

                        if constexpr (HasCommands) {
                            tasks.push_back(Async::Scheduler::spawn(
                                [fn, chunks = std::move(chunks), resources, command_buffer]() mutable noexcept {
                                    const F &system = fn;
                                    Commands commands = command_buffer->view();
                                    for (const auto &chunk : chunks) {
                                        chunk.each([&](Entity entity, Ts &...components) noexcept {
                                            invoke_with_commands(system, resources, commands, entity, components...);
                                        });
                                    }
                                }));
                        } else {
                            tasks.push_back(Async::Scheduler::spawn(
                                [fn, chunks = std::move(chunks), resources]() mutable noexcept {
                                    const F &system = fn;
                                    for (const auto &chunk : chunks) {
                                        chunk.each([&](Entity entity, Ts &...components) noexcept {
                                            invoke_without_commands(system, resources, entity, components...);
                                        });
                                    }
                                }));
                        }
                        return;
                    }

                    for (auto &chunk : chunks) {
                        CommandBuffer *command_buffer = nullptr;
                        if constexpr (HasCommands) {
                            command_buffers.emplace_back();
                            command_buffer = &command_buffers.back();
                        }

                        if constexpr (HasCommands) {
                            tasks.push_back(Async::Scheduler::spawn(
                                [fn, chunk = std::move(chunk), resources, command_buffer]() mutable noexcept {
                                    const F &system = fn;
                                    Commands commands = command_buffer->view();
                                    chunk.each([&](Entity entity, Ts &...components) noexcept {
                                        invoke_with_commands(system, resources, commands, entity, components...);
                                    });
                                }));
                        } else {
                            tasks.push_back(Async::Scheduler::spawn(
                                [fn, chunk = std::move(chunk), resources]() mutable noexcept {
                                    const F &system = fn;
                                    chunk.each([&](Entity entity, Ts &...components) noexcept {
                                        invoke_without_commands(system, resources, entity, components...);
                                    });
                                }));
                        }
                    }
                };
            }
        };

    } // namespace Detail

    // Safe systems are per-entity noexcept callables:
    //   [](Entity, Position&, const Velocity&) noexcept { ... }
    //   [](Entity, const Mesh&, WriteResource<RenderRequests>) noexcept { ... }
    //   [](Entity, const Health&, Commands&) noexcept { commands.destroy(entity); }
    //
    // Component reference constness is the access declaration. Schedule derives conflicts, splits
    // matching archetypes into disjoint chunks, dispatches every chunk through Async, waits at the
    // dependency-stage boundary, then applies deferred Commands. ReadResource/WriteResource parameters
    // declare singleton access; mutable resources serialize that system's chunks automatically. No
    // mutable World reference enters worker code. A callable is copied once per chunk; captures shared
    // by reference remain the consumer's synchronization responsibility.
    class Schedule {
      public:
        explicit Schedule(ScheduleConfig config = {}) noexcept : config_(config) {}

        template <class F>
        void add_system(F fn) {
            using Function = std::decay_t<F>;
            static_assert(std::copy_constructible<Function>,
                          "Automatically parallel ECS systems must be copy-constructible.");

            using ArgsTuple = typename Detail::CallableTraits<Function>::ArgsTuple;
            using Traits = Detail::EntitySystemTraits<ArgsTuple>;
            using Runner = Detail::EntitySystemRunner<typename Traits::QueryType,
                                                      typename Traits::ResourceArguments,
                                                      Traits::HasCommands>;
            static_assert(Runner::template valid_callback<Function>(),
                          "ECS systems must be noexcept, return void, and match (Entity, Components&... [, Commands&]).");

            SystemEntry entry;
            entry.access = Detail::QueryAccessOf<typename Traits::QueryType>::access();
            Detail::ResourceAccessOf<typename Traits::ResourceArguments>::accumulate(entry.access);
            entry.dispatch = Runner::make_dispatch(std::move(fn));
            systems_.push_back(std::move(entry));
            stages_dirty_ = true;
        }

        void run(World &world);

      private:
        struct SystemEntry {
            SystemAccess access;
            Detail::SystemDispatch dispatch;
        };

        void rebuild_stages();

        ScheduleConfig config_{};
        std::vector<SystemEntry> systems_;
        std::vector<std::vector<usize>> stages_;
        bool stages_dirty_ = true;
    };

} // namespace SFT::Ecs
