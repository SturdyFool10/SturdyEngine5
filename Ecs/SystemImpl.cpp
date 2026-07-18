#include <Ecs/System.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <shared_mutex>

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
