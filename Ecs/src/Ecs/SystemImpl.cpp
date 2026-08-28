#include <Ecs/System.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <tracy/Tracy.hpp>

namespace SFT::Ecs {

    namespace {

        class ScheduledWorldScope {
          public:
            /// Constructs a `ScheduledWorldScope` from the supplied initialization values.
            ///
            /// @param world World used or affected by the operation.
            ///
            /// @note This function does not throw exceptions.
            explicit ScheduledWorldScope(World &world) noexcept
                : world_(&world), access_(Detail::WorldAccess::begin_schedule(world)) {}

            /// Destroys the `ScheduledWorldScope` and releases resources owned by it.
            ///
            /// @note This function does not throw exceptions.
            ~ScheduledWorldScope() noexcept {
                Detail::WorldAccess::end_schedule(*world_);
            }

            /// Disables this construction form for `ScheduledWorldScope`.
            ///
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            ScheduledWorldScope(const ScheduledWorldScope &) = delete;
            /// Assigns a new value to this `ScheduledWorldScope`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            ScheduledWorldScope &operator=(const ScheduledWorldScope &) = delete;

          private:
            World *world_;
            std::unique_lock<std::shared_mutex> access_;
        };

    } // namespace


    /// Returns the current or globally available rebuild stages value.
    ///
    /// @return Returns the current rebuild stages value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Schedule::rebuild_stages() {
        ZoneScopedN("Schedule::rebuild_stages");
        stages_.clear();
        std::vector<usize> remaining(systems_.size());
        for (usize i = 0; i < systems_.size(); ++i) {
            remaining[i] = i;
        }

        while (!remaining.empty()) {
            std::vector<usize> stage;
            std::vector<usize> next_remaining;
            for (usize index : remaining) {
                bool conflicts = false;


                for (ResourceKey read_event : systems_[index].access.event_reads) {
                    for (usize pending_index : remaining) {
                        if (pending_index >= index) continue;
                        const auto &writes = systems_[pending_index].access.event_writes;
                        if (std::find(writes.begin(), writes.end(), read_event) != writes.end()) {
                            conflicts = true;
                            break;
                        }
                    }
                    if (conflicts) break;
                }
                for (usize placed_index : stage) {
                    if (system_access_conflicts(systems_[index].access, systems_[placed_index].access)) {
                        conflicts = true;
                        break;
                    }
                }
                if (conflicts) {
                    next_remaining.push_back(index);
                } else {
                    stage.push_back(index);
                }
            }
            stages_.push_back(std::move(stage));
            remaining = std::move(next_remaining);
        }
        stages_dirty_ = false;
        validate_event_ordering();
    }


    /// Validates event ordering.
    ///
    /// @return Returns the current validate event ordering value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Schedule::validate_event_ordering() const {
        ZoneScopedN("Schedule::validate_event_ordering");
        if (!config_.clear_events_on_run) return;
        std::unordered_map<ResourceKey, usize, ResourceKeyHash> max_writer_stage;
        for (usize stage_index = 0; stage_index < stages_.size(); ++stage_index) {
            for (usize system_index : stages_[stage_index]) {
                for (ResourceKey key : systems_[system_index].access.event_writes) {
                    auto [entry, inserted] = max_writer_stage.try_emplace(key, stage_index);
                    if (!inserted && entry->second < stage_index) {
                        entry->second = stage_index;
                    }
                }
            }
        }
        if (max_writer_stage.empty()) {
            return;
        }
        for (usize stage_index = 0; stage_index < stages_.size(); ++stage_index) {
            for (usize system_index : stages_[stage_index]) {
                for (ResourceKey key : systems_[system_index].access.event_reads) {
                    const auto writer = max_writer_stage.find(key);
                    if (writer != max_writer_stage.end() && stage_index <= writer->second) {
                        Detail::contract_violation(
                            "ECS event ordering: an EventReader system was registered (add_system) before "
                            "every EventWriter system for the same event type, so it would see zero events "
                            "every tick. Register EventWriter systems before EventReader systems for the "
                            "same event type.");
                    }
                }
            }
        }
    }

    /// Runs the requested work.
    ///
    /// @param world World used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    /// Registers a system whose body is a plain function pointer.
    ///
    /// @param access Declared component and resource access.
    /// @param component_ids Components an entity must all carry to be visited.
    /// @param fn System body.
    /// @param user_data Passed through to every callback.
    /// @param prepare Optional per-dispatch setup.
    /// @param finish Optional per-dispatch teardown.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Schedule::add_erased_system(SystemAccess access,
                                     std::vector<ComponentId> component_ids,
                                     ErasedSystemFn fn,
                                     void *user_data,
                                     ErasedSystemPrepareFn prepare,
                                     ErasedSystemFinishFn finish) {
        ZoneScopedN("Schedule::add_erased_system");
        if (fn == nullptr || component_ids.empty()) {
            return;
        }

        SystemEntry entry;
        entry.access = std::move(access);
        entry.dispatch = [ids = std::move(component_ids), fn, user_data, prepare, finish](
                             World &world,
                             usize,
                             usize,
                             ExecutorPolicy,
                             Detail::AsyncTaskList &,
                             Detail::CommandBufferList &command_buffers) mutable {
            ZoneScopedN("Schedule::erased_system_dispatch");

            // A command buffer is allocated unconditionally rather than on demand: unlike a typed
            // system, whose signature says whether it wants Commands, a function pointer gives no
            // such hint, and the buffer costs nothing when nothing is queued into it.
            command_buffers.emplace_back();
            Detail::CommandBuffer &buffer = command_buffers.back();
            Commands commands = buffer.view();

            void *dispatch_context = prepare != nullptr ? prepare(&commands, user_data) : nullptr;

            struct DispatchContext {
                ErasedSystemFn fn;
                void *dispatch_context;
                void *user_data;
            };
            DispatchContext context{fn, dispatch_context, user_data};

            // Runs inline on the scheduling thread. Splitting an erased system across workers would
            // need the caller to promise its body is safe to run concurrently against itself, which
            // the current declaration has no way to express.
            (void)Detail::WorldAccess::for_each_scheduled(
                world, ids,
                [](Entity entity, void **components, void *context_pointer) noexcept {
                    auto *dispatch = static_cast<DispatchContext *>(context_pointer);
                    dispatch->fn(entity, components, dispatch->dispatch_context, dispatch->user_data);
                },
                &context);

            // Run unconditionally, including when nothing matched, so whatever prepare acquired is
            // always released.
            if (finish != nullptr) {
                finish(dispatch_context, user_data);
            }
        };
        systems_.push_back(std::move(entry));
        stages_dirty_ = true;
    }

    /// Registers a system that runs once per frame rather than once per entity.
    ///
    /// @param access Declared resource and event access.
    /// @param fn Body to run once per dispatch.
    /// @param user_data Passed through to every callback.
    /// @param prepare Optional per-dispatch setup.
    /// @param finish Optional per-dispatch teardown.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void Schedule::add_erased_global_system(SystemAccess access,
                                            ErasedSystemFn fn,
                                            void *user_data,
                                            ErasedSystemPrepareFn prepare,
                                            ErasedSystemFinishFn finish) {
        ZoneScopedN("Schedule::add_erased_global_system");
        if (fn == nullptr) {
            return;
        }

        SystemEntry entry;
        entry.access = std::move(access);
        entry.dispatch = [fn, user_data, prepare, finish](World &,
                                                          usize,
                                                          usize,
                                                          ExecutorPolicy,
                                                          Detail::AsyncTaskList &,
                                                          Detail::CommandBufferList &command_buffers) mutable {
            ZoneScopedN("Schedule::erased_global_system_dispatch");

            command_buffers.emplace_back();
            Detail::CommandBuffer &buffer = command_buffers.back();
            Commands commands = buffer.view();

            void *dispatch_context = prepare != nullptr ? prepare(&commands, user_data) : nullptr;

            // A default-constructed entity and a null component array: there is no entity to report
            // and nothing to point at, and the body was registered knowing that.
            fn(Entity{}, nullptr, dispatch_context, user_data);

            if (finish != nullptr) {
                finish(dispatch_context, user_data);
            }
        };
        systems_.push_back(std::move(entry));
        stages_dirty_ = true;
    }

    void Schedule::run(World &world) {
        ZoneScopedN("Schedule::run");


        usize target_parallelism = 1;
        if (config_.executor == ExecutorPolicy::Async) {
            if (Async::Scheduler::is_worker_thread()) {
                Detail::contract_violation(
                    "ECS Schedule::run() must be called from a coordinating non-worker thread; blocking a worker would deadlock nested Async work.");
            }
            if (!Async::Scheduler::is_running()) {
                Async::Scheduler::initialize();
            }
            const usize worker_count = std::max<usize>(1, Async::Scheduler::worker_count());
            const usize tasks_per_worker = std::max<usize>(1, config_.tasks_per_worker);
            target_parallelism = worker_count > std::numeric_limits<usize>::max() / tasks_per_worker
                                     ? std::numeric_limits<usize>::max()
                                     : worker_count * tasks_per_worker;
        }
        if (stages_dirty_) {
            rebuild_stages();
        }

        const usize minimum_rows_per_task = std::max<usize>(1, config_.minimum_rows_per_task);

        ScheduledWorldScope scheduled_world{world};
        if (config_.clear_events_on_run) {
            Detail::WorldAccess::clear_event_resources(world);
        }
        for (const std::vector<usize> &stage : stages_) {
            Detail::AsyncTaskList tasks;
            Detail::CommandBufferList command_buffers;

            for (usize system_index : stage) {
                systems_[system_index].dispatch(world,
                                                minimum_rows_per_task,
                                                target_parallelism,
                                                config_.executor,
                                                tasks,
                                                command_buffers);
            }
            for (Async::TaskHandle<void> &task : tasks) {
                task.wait();
            }
            for (Detail::CommandBuffer &command_buffer : command_buffers) {
                command_buffer.apply(world);
            }
        }
    }

} // namespace SFT::Ecs
