#pragma once

#include <Async/Scheduler.hpp>
#include <Async/Task.hpp>

#include <Ecs/Commands.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Event.hpp>
#include <Ecs/Query.hpp>
#include <Ecs/Resource.hpp>
#include <Ecs/World.hpp>

#include <concepts>
#include <deque>
#include <functional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {


    /// Prepares one dispatch of an erased system, before any entity is visited.
    ///
    /// Receives the `Commands` queue for this dispatch and returns an opaque context handed to
    /// every per-entity call and finally to the finish callback. This exists so a binding can do
    /// per-dispatch setup once — the C FFI mints its `Commands` handle here — instead of repeating
    /// it for every entity on a hot path.
    ///
    /// May be null, in which case the per-entity context is null.
    using ErasedSystemPrepareFn = void *(*)(Commands *commands, void *user_data) noexcept;

    /// Body of a system registered through `Schedule::add_erased_system`, called once per entity.
    ///
    /// `components` points at an array parallel to the component ids the system was registered
    /// with, each entry addressing that component's storage for this entity, valid only for this
    /// call. `dispatch_context` is whatever the prepare callback returned.
    using ErasedSystemFn = void (*)(Entity entity,
                                    void **components,
                                    void *dispatch_context,
                                    void *user_data) noexcept;

    /// Tears down one dispatch of an erased system, after the last entity is visited.
    ///
    /// Runs even when no entity matched, so whatever prepare acquired is always released. May be
    /// null.
    using ErasedSystemFinishFn = void (*)(void *dispatch_context, void *user_data) noexcept;

    struct SystemAccess {
        std::vector<ComponentKey> reads;
        std::vector<ComponentKey> writes;
        std::vector<ResourceKey> resource_reads;
        std::vector<ResourceKey> resource_writes;
        std::vector<ResourceKey> event_reads;
        std::vector<ResourceKey> event_writes;
    };


    /// Returns the current or globally available access sets conflict value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    template <class Key>
    [[nodiscard]] inline bool access_sets_conflict(const std::vector<Key> &a_reads,
                                                   const std::vector<Key> &a_writes,
                                                   const std::vector<Key> &b_reads,
                                                   const std::vector<Key> &b_writes) noexcept {
        ZoneScopedN("access_sets_conflict");
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

    /// Performs the system access conflicts operation using the supplied arguments.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool system_access_conflicts(const SystemAccess &a, const SystemAccess &b) noexcept;


    enum class ExecutorPolicy {
        Async,
        Synchronous,
    };

    struct ScheduleConfig {


        usize minimum_rows_per_task = 128;
        usize tasks_per_worker = 2;


        bool clear_events_on_run = true;
        ExecutorPolicy executor = ExecutorPolicy::Async;
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
            /// Returns the current or globally available access value.
            ///
            /// @return Returns the current access value.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] static SystemAccess access() {
                SystemAccess result;
                (accumulate<Ts>(result), ...);
                return result;
            }

          private:
            /// Performs the accumulate operation for `QueryAccessOf` using the supplied arguments.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
            /// Reports whether this `ResourceAccessOf` has writes.
            ///
            /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] static constexpr bool has_writes() noexcept {
                return (ResourceArgumentTraits<ResourceArgs>::IsWrite || ... || false);
            }

            /// Performs the accumulate operation for `ResourceAccessOf` using the supplied arguments.
            ///
            /// @param result `result` value used by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            static void accumulate(SystemAccess &result) {
                (accumulate_one<ResourceArgs>(result), ...);
            }

          private:
            /// Performs the accumulate one operation for `ResourceAccessOf` using the supplied arguments.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


        /// Resolves resource argument into the concrete value used by the engine.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <class Argument>
        [[nodiscard]] Argument resolve_resource_argument(World &world) noexcept {
            using Traits = ResourceArgumentTraits<Argument>;
            using Resource = typename Traits::Resource;
            return Traits::construct(WorldAccess::resource<Resource>(world));
        }

        /// Resolves resource arguments into the concrete value used by the engine.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <class... ResourceArgs>
        [[nodiscard]] std::tuple<ResourceArgs...> resolve_resource_arguments(World &world) noexcept {
            return std::tuple<ResourceArgs...>{resolve_resource_argument<ResourceArgs>(world)...};
        }

        template <class ArgsTuple, class Indices>
        struct QueryFromSystemArguments;

        template <class ArgsTuple, usize... Is>
        struct QueryFromSystemArguments<ArgsTuple, std::index_sequence<Is...>> {


            using Type = Query<std::remove_reference_t<std::tuple_element_t<Is + 1, ArgsTuple>>...>;
        };

        template <class ArgsTuple, usize Offset, class Indices>
        struct TupleSlice;

        template <class ArgsTuple, usize Offset, usize... Is>
        struct TupleSlice<ArgsTuple, Offset, std::index_sequence<Is...>> {
            using Type = std::tuple<std::tuple_element_t<Offset + Is, ArgsTuple>...>;
        };


        /// Computes args begin with entity using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Returns the component prefix count for this `EntitySystemTraits`.
            ///
            /// @return Returns the requested count or size.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Reports whether valid component arguments is valid for the current operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <usize... Is>
            [[nodiscard]] static consteval bool valid_component_arguments(std::index_sequence<Is...>) {
                return ((std::is_lvalue_reference_v<std::tuple_element_t<Is + 1, ArgsTuple>> &&
                         !std::is_volatile_v<std::remove_reference_t<std::tuple_element_t<Is + 1, ArgsTuple>>> &&
                         !std::same_as<std::remove_cvref_t<std::tuple_element_t<Is + 1, ArgsTuple>>, Commands>) &&
                        ...);
            }

            /// Reports whether valid resource arguments is valid for the current operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
        using SystemDispatch =
            std::function<void(World &, usize, usize, ExecutorPolicy, AsyncTaskList &, CommandBufferList &)>;


        /// Dispatches task.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class F>
        void dispatch_task(ExecutorPolicy policy, AsyncTaskList &tasks, F &&fn) {
            ZoneScopedN("dispatch_task");
            if (policy == ExecutorPolicy::Synchronous) {
                std::forward<F>(fn)();
            } else {
                tasks.push_back(Async::Scheduler::spawn(std::forward<F>(fn)));
            }
        }

        template <class QueryType, class ResourceTuple, bool HasCommands>
        struct EntitySystemRunner;

        template <class... Ts, class... ResourceArgs, bool HasCommands>
        struct EntitySystemRunner<Query<Ts...>, std::tuple<ResourceArgs...>, HasCommands> {
            /// Reports whether valid callback is valid for the current operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Performs the invoke without commands operation for `EntitySystemRunner` using the supplied arguments.
            ///
            /// @note This function does not throw exceptions.
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

            /// Performs the invoke with commands operation for `EntitySystemRunner` using the supplied arguments.
            ///
            /// @note This function does not throw exceptions.
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

            /// Creates a dispatch value from the supplied arguments.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class F>
            [[nodiscard]] static SystemDispatch make_dispatch(F fn) {
                return [fn = std::move(fn)](World &world,
                                            usize minimum_rows_per_task,
                                            usize target_parallelism,
                                            ExecutorPolicy policy,
                                            AsyncTaskList &tasks,
                                            CommandBufferList &command_buffers) mutable {
                    ZoneScopedN("EntitySystemRunner::dispatch");
                    auto query = WorldAccess::query<Ts...>(world);
                    auto chunks = query.chunks(minimum_rows_per_task, target_parallelism);
                    auto resources = resolve_resource_arguments<ResourceArgs...>(world);


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
                            dispatch_task(policy, tasks,
                                [fn, chunks = std::move(chunks), resources, command_buffer]() mutable noexcept {
                                    const F &system = fn;
                                    Commands commands = command_buffer->view();
                                    for (const auto &chunk : chunks) {
                                        chunk.each([&](Entity entity, Ts &...components) noexcept {
                                            invoke_with_commands(system, resources, commands, entity, components...);
                                        });
                                    }
                                });
                        } else {
                            dispatch_task(policy, tasks,
                                [fn, chunks = std::move(chunks), resources]() mutable noexcept {
                                    const F &system = fn;
                                    for (const auto &chunk : chunks) {
                                        chunk.each([&](Entity entity, Ts &...components) noexcept {
                                            invoke_without_commands(system, resources, entity, components...);
                                        });
                                    }
                                });
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
                            dispatch_task(policy, tasks,
                                [fn, chunk = std::move(chunk), resources, command_buffer]() mutable noexcept {
                                    const F &system = fn;
                                    Commands commands = command_buffer->view();
                                    chunk.each([&](Entity entity, Ts &...components) noexcept {
                                        invoke_with_commands(system, resources, commands, entity, components...);
                                    });
                                });
                        } else {
                            dispatch_task(policy, tasks,
                                [fn, chunk = std::move(chunk), resources]() mutable noexcept {
                                    const F &system = fn;
                                    chunk.each([&](Entity entity, Ts &...components) noexcept {
                                        invoke_without_commands(system, resources, entity, components...);
                                    });
                                });
                        }
                    }
                };
            }
        };


        template <class ArgsTuple>
        struct GlobalSystemTraits {
            static constexpr usize ArgumentCount = std::tuple_size_v<ArgsTuple>;

            /// Reports whether compute has commands.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Reports whether valid resource arguments is valid for the current operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
            /// Reports whether valid callback is valid for the current operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

            /// Creates a dispatch value from the supplied arguments.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <class F>
            [[nodiscard]] static SystemDispatch make_dispatch(F fn) {
                return [fn = std::move(fn)](World &world,
                                            usize                          ,
                                            usize                       ,
                                            ExecutorPolicy policy,
                                            AsyncTaskList &tasks,
                                            CommandBufferList &command_buffers) mutable {
                    ZoneScopedN("GlobalSystemRunner::dispatch");
                    auto resources = resolve_resource_arguments<ResourceArgs...>(world);
                    CommandBuffer *command_buffer = nullptr;
                    if constexpr (HasCommands) {
                        command_buffers.emplace_back();
                        command_buffer = &command_buffers.back();
                    }

                    if constexpr (HasCommands) {
                        dispatch_task(policy, tasks,
                            [fn, resources, command_buffer]() mutable noexcept {
                                const F &system = fn;
                                Commands commands = command_buffer->view();
                                std::apply(
                                    [&](ResourceArgs... resource_views) noexcept {
                                        std::invoke(system, resource_views..., commands);
                                    },
                                    resources);
                            });
                    } else {
                        dispatch_task(policy, tasks,
                            [fn, resources]() mutable noexcept {
                                const F &system = fn;
                                std::apply(
                                    [&](ResourceArgs... resource_views) noexcept {
                                        std::invoke(system, resource_views...);
                                    },
                                    resources);
                            });
                    }
                };
            }
        };

    } // namespace Detail


    class Schedule {
      public:
        /// Constructs a `Schedule` from the supplied initialization values.
        ///
        /// @param config Configuration values controlling the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit Schedule(ScheduleConfig config = {}) noexcept;


        /// Adds system using the supplied arguments and current state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class F>
        void add_system(F fn) {
            ZoneScopedN("Schedule::add_system");
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

        /// Registers a system whose body is a plain function pointer, with its data access declared
        /// explicitly rather than deduced from C++ parameter types.
        ///
        /// This is what lets a system be written in another language. `add_system` above reads a
        /// callable's signature to work out which components it reads and writes, which is how the
        /// scheduler knows what may run in parallel; a C function pointer carries no such
        /// information, so the caller has to supply it. Getting that declaration wrong is the one
        /// way to defeat the scheduler's safety, so a caller that is unsure should declare a write.
        ///
        /// The system runs synchronously on the scheduling thread rather than being split across
        /// workers. Systems in the same stage still run concurrently with each other where their
        /// declared access allows it; only this system's own entities are not yet subdivided.
        ///
        /// @param access Declared component and resource access, used for conflict detection.
        /// @param component_ids Components an entity must all carry for this system to see it.
        ///        Must be consistent with `access` for the scheduling to mean anything.
        /// @param fn System body. Must not throw or unwind.
        /// @param user_data Passed through to every callback untouched.
        /// @param prepare Optional per-dispatch setup receiving this dispatch's `Commands`.
        /// @param finish Optional per-dispatch teardown, always run if `prepare` ran.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void add_erased_system(SystemAccess access,
                               std::vector<ComponentId> component_ids,
                               ErasedSystemFn fn,
                               void *user_data,
                               ErasedSystemPrepareFn prepare = nullptr,
                               ErasedSystemFinishFn finish = nullptr);

        /// Registers a system that runs once per frame rather than once per entity.
        ///
        /// The erased counterpart of a resource-only typed system. Declaring no components means
        /// there is nothing to iterate, so the body runs exactly once per dispatch — the right shape
        /// for a system that only reads events or drives a resource.
        ///
        /// @param access Declared resource and event access, used for conflict detection.
        /// @param fn Body to run once per dispatch. Must not throw or unwind.
        /// @param user_data Passed through to every callback untouched.
        /// @param prepare Optional per-dispatch setup receiving this dispatch's `Commands`.
        /// @param finish Optional per-dispatch teardown, always run if `prepare` ran.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void add_erased_global_system(SystemAccess access,
                                      ErasedSystemFn fn,
                                      void *user_data,
                                      ErasedSystemPrepareFn prepare = nullptr,
                                      ErasedSystemFinishFn finish = nullptr);

        /// Runs the requested work.
        ///
        /// @param world World used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void run(World &world);

      private:
        struct SystemEntry {
            SystemAccess access;
            Detail::SystemDispatch dispatch;
        };

        /// Performs the rebuild stages operation for `Schedule` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void rebuild_stages();
        /// Validates event ordering.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void validate_event_ordering() const;

        ScheduleConfig config_{};
        std::vector<SystemEntry> systems_;
        std::vector<std::vector<usize>> stages_;
        bool stages_dirty_ = true;
    };

} // namespace SFT::Ecs
