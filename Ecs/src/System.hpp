#pragma once

#include <Async/src/Scheduler.hpp>
#include <Async/src/Task.hpp>

#include <Ecs/src/Commands.hpp>
#include <Ecs/src/Component.hpp>
#include <Ecs/src/Event.hpp>
#include <Ecs/src/Query.hpp>
#include <Ecs/src/Resource.hpp>
#include <Ecs/src/World.hpp>

#include <concepts>
#include <deque>
#include <functional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SFT::Ecs {

    // Derived from a safe per-entity system's component-reference parameters. Stable keys make the
    // declaration independent of one World's dense registry IDs. event_reads/event_writes are a
    // subset of resource_reads/resource_writes (populated only for Events<T> resources) that
    // Schedule uses solely for the producer-before-consumer ordering check below — they play no part
    // in system_access_conflicts, which already treats an event buffer as an ordinary resource.
    struct SystemAccess {
        std::vector<ComponentKey> reads;
        std::vector<ComponentKey> writes;
        std::vector<ResourceKey> resource_reads;
        std::vector<ResourceKey> resource_writes;
        std::vector<ResourceKey> event_reads;
        std::vector<ResourceKey> event_writes;
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
                constexpr ResourceKey key = resource_key<Resource>();
                if constexpr (Traits::IsWrite) {
                    result.resource_writes.push_back(key);
                    if constexpr (Traits::IsEvent) {
                        result.event_writes.push_back(key);
                    }
                } else {
                    result.resource_reads.push_back(key);
                    if constexpr (Traits::IsEvent) {
                        result.event_reads.push_back(key);
                    }
                }
            }
        };

        // Resolves one resource-view argument (ReadResource/WriteResource/EventReader/EventWriter)
        // through its ResourceArgumentTraits::construct() seam — the one place that needs to know
        // about a new resource-view kind is that trait specialization, not here.
        template <class Argument>
        [[nodiscard]] Argument resolve_resource_argument(World &world) noexcept {
            using Traits = ResourceArgumentTraits<Argument>;
            using Resource = typename Traits::Resource;
            return Traits::construct(WorldAccess::resource<Resource>(world));
        }

        template <class... ResourceArgs>
        [[nodiscard]] std::tuple<ResourceArgs...> resolve_resource_arguments(World &world) noexcept {
            return std::tuple<ResourceArgs...>{resolve_resource_argument<ResourceArgs>(world)...};
        }

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

        // Distinguishes the two system shapes add_system() accepts: an entity-and-component system
        // always starts with Entity by value; a resource-only ("global") system never does, since it
        // has no query to iterate. Checked on the raw (possibly empty) ArgsTuple so it's safe to use
        // before EntitySystemTraits's own ArgumentCount>=2 assertion would apply. Written with
        // if constexpr rather than a plain && — tuple_element_t<0, ArgsTuple> is ill-formed for an
        // empty ArgsTuple, and unlike runtime short-circuiting, a bare && still requires both operand
        // types to be valid.
        template <class ArgsTuple>
        [[nodiscard]] consteval bool compute_args_begin_with_entity() {
            if constexpr (std::tuple_size_v<ArgsTuple> == 0) {
                return false;
            } else {
                return std::same_as<std::tuple_element_t<0, ArgsTuple>, Entity>;
            }
        }

        template <class ArgsTuple>
        inline constexpr bool args_begin_with_entity_v = compute_args_begin_with_entity<ArgsTuple>();

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
                    auto resources = resolve_resource_arguments<ResourceArgs...>(world);

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

        // A "global" system: resource/event access only, no Entity, no component query — the shape
        // an event-producing system like a hotkey mapper needs (it has no entities to iterate). Mirrors
        // EntitySystemTraits minus everything query-related.
        template <class ArgsTuple>
        struct GlobalSystemTraits {
            static constexpr usize ArgumentCount = std::tuple_size_v<ArgsTuple>;

            [[nodiscard]] static consteval bool compute_has_commands() {
                if constexpr (ArgumentCount == 0) {
                    return false;
                } else {
                    using Last = std::tuple_element_t<ArgumentCount - 1, ArgsTuple>;
                    return std::same_as<std::remove_cvref_t<Last>, Commands>;
                }
            }

            static constexpr bool HasCommands = compute_has_commands();
            static_assert(!HasCommands || std::same_as<std::tuple_element_t<ArgumentCount - 1, ArgsTuple>, Commands &>,
                          "Commands must be the final system parameter and must be passed as Commands&.");

            static constexpr usize ResourceCount = ArgumentCount - static_cast<usize>(HasCommands);
            using ResourceIndices = std::make_index_sequence<ResourceCount>;

            template <usize... Is>
            [[nodiscard]] static consteval bool valid_resource_arguments(std::index_sequence<Is...>) {
                return ((is_resource_argument_v<std::tuple_element_t<Is, ArgsTuple>> &&
                         std::same_as<std::tuple_element_t<Is, ArgsTuple>,
                                      std::remove_cvref_t<std::tuple_element_t<Is, ArgsTuple>>>) &&
                        ...);
            }

            static_assert(valid_resource_arguments(ResourceIndices{}),
                          "A resource-only ECS system must declare only ReadResource<T>/WriteResource<T>/"
                          "EventReader<T>/EventWriter<T> parameters (optionally followed by Commands&).");
            static_assert(ResourceCount > 0,
                          "A resource-only ECS system must declare at least one resource/event parameter.");

            using ResourceArguments = typename TupleSlice<ArgsTuple, 0, ResourceIndices>::Type;
        };

        template <class ResourceTuple, bool HasCommands>
        struct GlobalSystemRunner;

        template <class... ResourceArgs, bool HasCommands>
        struct GlobalSystemRunner<std::tuple<ResourceArgs...>, HasCommands> {
            template <class F>
            [[nodiscard]] static consteval bool valid_callback() {
                if constexpr (HasCommands) {
                    if constexpr (std::is_nothrow_invocable_v<const F &, ResourceArgs..., Commands &>) {
                        return std::same_as<std::invoke_result_t<const F &, ResourceArgs..., Commands &>, void>;
                    } else {
                        return false;
                    }
                } else {
                    if constexpr (std::is_nothrow_invocable_v<const F &, ResourceArgs...>) {
                        return std::same_as<std::invoke_result_t<const F &, ResourceArgs...>, void>;
                    } else {
                        return false;
                    }
                }
            }

            template <class F>
            [[nodiscard]] static SystemDispatch make_dispatch(F fn) {
                return [fn = std::move(fn)](World &world,
                                            usize /*minimum_rows_per_task*/,
                                            usize /*target_parallelism*/,
                                            AsyncTaskList &tasks,
                                            CommandBufferList &command_buffers) mutable {
                    auto resources = resolve_resource_arguments<ResourceArgs...>(world);
                    CommandBuffer *command_buffer = nullptr;
                    if constexpr (HasCommands) {
                        command_buffers.emplace_back();
                        command_buffer = &command_buffers.back();
                    }

                    if constexpr (HasCommands) {
                        tasks.push_back(Async::Scheduler::spawn(
                            [fn, resources, command_buffer]() mutable noexcept {
                                const F &system = fn;
                                Commands commands = command_buffer->view();
                                std::apply(
                                    [&](ResourceArgs... resource_views) noexcept {
                                        std::invoke(system, resource_views..., commands);
                                    },
                                    resources);
                            }));
                    } else {
                        tasks.push_back(Async::Scheduler::spawn(
                            [fn, resources]() mutable noexcept {
                                const F &system = fn;
                                std::apply(
                                    [&](ResourceArgs... resource_views) noexcept {
                                        std::invoke(system, resource_views...);
                                    },
                                    resources);
                            }));
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

        // Accepts either system shape: (Entity, Components&..., [ResourceArgs...], [Commands&]) for
        // per-entity work, or (ResourceArgs..., [Commands&]) for resource/event-only ("global") work
        // such as an event-producing hotkey mapper. The two are told apart purely from the callable's
        // own first parameter — Entity or not — matching this codebase's "derive it, don't hand-specify
        // it" rule elsewhere (query const-ness, resource access).
        template <class F>
        void add_system(F fn) {
            using Function = std::decay_t<F>;
            static_assert(std::copy_constructible<Function>,
                          "Automatically parallel ECS systems must be copy-constructible.");

            using ArgsTuple = typename Detail::CallableTraits<Function>::ArgsTuple;

            if constexpr (Detail::args_begin_with_entity_v<ArgsTuple>) {
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
            } else {
                using Traits = Detail::GlobalSystemTraits<ArgsTuple>;
                using Runner = Detail::GlobalSystemRunner<typename Traits::ResourceArguments, Traits::HasCommands>;
                static_assert(Runner::template valid_callback<Function>(),
                              "Resource-only ECS systems must be noexcept, return void, and match "
                              "(ResourceArgs... [, Commands&]).");

                SystemEntry entry;
                Detail::ResourceAccessOf<typename Traits::ResourceArguments>::accumulate(entry.access);
                entry.dispatch = Runner::make_dispatch(std::move(fn));
                systems_.push_back(std::move(entry));
            }
            stages_dirty_ = true;
        }

        void run(World &world);

      private:
        struct SystemEntry {
            SystemAccess access;
            Detail::SystemDispatch dispatch;
        };

        void rebuild_stages();
        void validate_event_ordering() const;

        ScheduleConfig config_{};
        std::vector<SystemEntry> systems_;
        std::vector<std::vector<usize>> stages_;
        bool stages_dirty_ = true;
    };

} // namespace SFT::Ecs
