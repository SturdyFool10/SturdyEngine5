#include <Ecs/src/System.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace SFT::Ecs {

    namespace {

        class ScheduledWorldScope {
          public:
            explicit ScheduledWorldScope(World &world) noexcept
                : world_(&world), access_(Detail::WorldAccess::begin_schedule(world)) {}

            ~ScheduledWorldScope() noexcept {
                Detail::WorldAccess::end_schedule(*world_);
            }

            ScheduledWorldScope(const ScheduledWorldScope &) = delete;
            ScheduledWorldScope &operator=(const ScheduledWorldScope &) = delete;

          private:
            World *world_;
            std::unique_lock<std::shared_mutex> access_;
        };

    } // namespace

    // Greedy, stable dependency staging. Every system placed in one stage is mutually non-conflicting;
    // conflicting systems retain their registration order across later stage boundaries.
    void Schedule::rebuild_stages() {
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
                // Event consumers may not leapfrog an earlier registered producer merely because that
                // producer was delayed by an unrelated component/resource conflict. Preserve the event
                // pipeline's registration order across stage construction, not just within one stage.
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

    // Events<T> is cleared at the top of every run() (World::clear_event_resources), so a reader
    // system staged at or before every writer of the same event type would see nothing that tick,
    // forever — not stale data, silently missing data. Stage placement is a pure function of
    // registration order (rebuild_stages() above), so this is really a registration-order contract:
    // register every EventWriter<T> system before any EventReader<T> system for the same T. Catch a
    // violation here, once, right after (re)building stages, instead of losing events at runtime.
    void Schedule::validate_event_ordering() const {
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

    void Schedule::run(World &world) {
        if (Async::Scheduler::is_worker_thread()) {
            Detail::contract_violation(
                "ECS Schedule::run() must be called from a coordinating non-worker thread; blocking a worker would deadlock nested Async work.");
        }
        if (!Async::Scheduler::is_running()) {
            Async::Scheduler::initialize();
        }
        if (stages_dirty_) {
            rebuild_stages();
        }

        const usize worker_count = std::max<usize>(1, Async::Scheduler::worker_count());
        const usize tasks_per_worker = std::max<usize>(1, config_.tasks_per_worker);
        const usize target_parallelism = worker_count > std::numeric_limits<usize>::max() / tasks_per_worker
                                             ? std::numeric_limits<usize>::max()
                                             : worker_count * tasks_per_worker;
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
