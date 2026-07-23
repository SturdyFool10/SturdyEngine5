#include <Foundation/src/Foundation.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <Async/src/Scheduler.hpp>
#include <Async/src/Mutex.hpp>


using std::condition_variable;
using std::deque;
using std::make_unique;
using std::thread;
using std::unique_lock;
using std::unique_ptr;
using std::vector;

namespace SFT::Async {

    namespace {

        // A single worker's task queue: push/pop from the back (owner), steal from the front
        // (thief). Built on `Mutex<T>` rather than a lock-free Chase-Lev deque — simpler to get
        // right, at the cost of a little contention under heavy stealing. A lock-free version is a
        // drop-in optimization behind this same interface if that ever shows up in a profile.
        class WorkerDeque {
          public:
            void push_back(unique_ptr<Detail::TaskBase> task) noexcept {
                auto guard = tasks_.lock();
                guard->push_back(std::move(task));
            }

            [[nodiscard]] unique_ptr<Detail::TaskBase> pop_back() noexcept {
                auto guard = tasks_.lock();
                if (guard->empty()) {
                    return nullptr;
                }
                unique_ptr<Detail::TaskBase> task = std::move(guard->back());
                guard->pop_back();
                return task;
            }

            [[nodiscard]] unique_ptr<Detail::TaskBase> steal() noexcept {
                auto guard = tasks_.lock();
                if (guard->empty()) {
                    return nullptr;
                }
                unique_ptr<Detail::TaskBase> task = std::move(guard->front());
                guard->pop_front();
                return task;
            }

            void clear() noexcept {
                auto guard = tasks_.lock();
                guard->clear();
            }

          private:
            Mutex<deque<unique_ptr<Detail::TaskBase>>> tasks_;
        };

        struct Pool {
            vector<thread> threads;
            vector<unique_ptr<WorkerDeque>> deques;
            WorkerDeque injector; // fallback queue fed by non-worker threads
            std::atomic<bool> running{false};
            // Tasks available in a deque, excluding work already executing. This keeps idle workers
            // asleep while the pool is fully occupied by long-running tasks.
            std::atomic<u32> queued_count{0};
            std::atomic<u32> waiting_worker_count{0};
            std::mutex wake_mutex;
            condition_variable wake_cv;
            SchedulerConfig config{};
        };

        Pool &pool() noexcept {
            static Pool instance;
            return instance;
        }

        // -1 on any thread that isn't a scheduler worker (the main thread, an app thread, ...).
        thread_local i32 t_worker_index = -1;
        thread_local u32 t_steal_cursor = 0;

        [[nodiscard]] unique_ptr<Detail::TaskBase> try_take_task(Pool &p, u32 index) noexcept {
            unique_ptr<Detail::TaskBase> task = p.deques[index]->pop_back();
            if (!task) {
                task = p.injector.steal();
            }
            if (!task && p.deques.size() > 1) {
                const auto worker_total = static_cast<u32>(p.deques.size());
                const u32 first_victim = t_steal_cursor;
                if (++t_steal_cursor == worker_total) {
                    t_steal_cursor = 0;
                }

                u32 victim = first_victim;
                for (u32 offset = 0; offset < worker_total && !task; ++offset) {
                    if (victim != index) {
                        task = p.deques[victim]->steal();
                    }
                    if (++victim == worker_total) {
                        victim = 0;
                    }
                }
            }
            return task;
        }

        [[nodiscard]] bool execute_one_task(Pool &p, u32 index) noexcept {
            unique_ptr<Detail::TaskBase> task = try_take_task(p, index);
            if (!task) {
                return false;
            }

            p.queued_count.fetch_sub(1, std::memory_order_acq_rel);
            task->execute();
            return true;
        }

        void worker_loop(u32 index) noexcept {
            t_worker_index = static_cast<i32>(index);
            Pool &p = pool();
            u32 idle_spins = 0;
            u32 idle_yields = 0;

            while (p.running.load(std::memory_order_acquire)) {
                if (execute_one_task(p, index)) {
                    idle_spins = 0;
                    idle_yields = 0;
                    continue;
                }

                const SchedulerConfig config = p.config;
                if (idle_spins < config.idle_spin_iterations) {
                    ++idle_spins;
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                    continue;
                }
                if (idle_yields < config.idle_yield_iterations) {
                    ++idle_yields;
                    std::this_thread::yield();
                    continue;
                }

                // Nothing anywhere right now — sleep briefly after a spin/yield grace period. The
                // predicate is re-checked on every loop iteration regardless, so a missed wakeup only
                // costs up to this timeout, never correctness.
                unique_lock<std::mutex> idle_lock(p.wake_mutex);
                p.wake_cv.wait_for(idle_lock, std::chrono::microseconds(config.idle_sleep_microseconds), [&p]() {
                    return !p.running.load(std::memory_order_acquire) || p.queued_count.load(std::memory_order_acquire) > 0;
                });
                idle_spins = 0;
                idle_yields = 0;
            }
        }

    } // namespace

    void Detail::notify_scheduler_task_completion() noexcept {
        Pool &p = pool();
        if (p.waiting_worker_count.load(std::memory_order_acquire) > 0) {
            p.wake_cv.notify_all();
        }
    }

    void Detail::wait_for_task(std::atomic<bool> &done) noexcept {
        if (done.load(std::memory_order_acquire)) {
            return;
        }
        if (t_worker_index < 0) {
            done.wait(false, std::memory_order_acquire);
            return;
        }

        Pool &p = pool();
        const u32 worker_index = static_cast<u32>(t_worker_index);
        p.waiting_worker_count.fetch_add(1, std::memory_order_acq_rel);
        while (!done.load(std::memory_order_acquire)) {
            if (execute_one_task(p, worker_index)) {
                continue;
            }

            // A worker cannot use done.wait() after an empty scan: work enqueued later may be
            // required to complete `done`, but enqueueing that work does not modify this atomic.
            unique_lock<std::mutex> idle_lock(p.wake_mutex);
            p.wake_cv.wait_for(
                idle_lock,
                std::chrono::microseconds(p.config.idle_sleep_microseconds),
                [&done, &p]() {
                    return done.load(std::memory_order_acquire) ||
                           p.queued_count.load(std::memory_order_acquire) > 0;
                });
        }
        p.waiting_worker_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    void Scheduler::initialize(u32 worker_count) noexcept {
        SchedulerConfig config{};
        config.worker_count = worker_count;
        initialize(config);
    }

    void Scheduler::initialize_low_latency(u32 worker_count) noexcept {
        SchedulerConfig config{};
        config.worker_count = worker_count;
        config.idle_spin_iterations = 1024;
        config.idle_yield_iterations = 128;
        config.idle_sleep_microseconds = 50;
        config.notify_all_on_enqueue = false;
        initialize(config);
    }

    void Scheduler::initialize(const SchedulerConfig &config) noexcept {
        Pool &p = pool();
        if (p.running.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        SchedulerConfig active_config = config;
        if (active_config.worker_count == 0) {
            const u32 hardware_threads = thread::hardware_concurrency();
            active_config.worker_count = hardware_threads > 1 ? hardware_threads - 1 : 1;
        }
        if (active_config.idle_sleep_microseconds == 0) {
            active_config.idle_sleep_microseconds = 1;
        }
        p.config = active_config;

        const u32 worker_count = active_config.worker_count;
        p.deques.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            p.deques.push_back(make_unique<WorkerDeque>());
        }

        p.threads.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            p.threads.emplace_back(worker_loop, i);
        }

        Foundation::log_info("Async::Scheduler started {} worker thread(s) [spin={}, yield={}, sleep={}us].",
                             worker_count,
                             active_config.idle_spin_iterations,
                             active_config.idle_yield_iterations,
                             active_config.idle_sleep_microseconds);
    }

    void Scheduler::shutdown() noexcept {
        Pool &p = pool();
        if (!p.running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        p.wake_cv.notify_all();
        for (thread &worker : p.threads) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        p.threads.clear();
        p.deques.clear();
        p.injector.clear();
        p.queued_count.store(0, std::memory_order_release);
    }

    bool Scheduler::is_running() noexcept {
        return pool().running.load(std::memory_order_acquire);
    }

    bool Scheduler::is_worker_thread() noexcept {
        return t_worker_index >= 0;
    }

    u32 Scheduler::worker_count() noexcept {
        return static_cast<u32>(pool().deques.size());
    }

    void Scheduler::enqueue(unique_ptr<Detail::TaskBase> task) noexcept {
        Pool &p = pool();
        if (!p.running.load(std::memory_order_acquire)) {
            initialize();
        }

        // Publish the count before the queue entry so a worker can never dequeue and decrement from
        // zero. Seeing a positive count slightly early is harmless: it is only an idle-wakeup hint.
        p.queued_count.fetch_add(1, std::memory_order_acq_rel);
        if (t_worker_index >= 0) {
            p.deques[static_cast<usize>(t_worker_index)]->push_back(std::move(task));
        } else {
            p.injector.push_back(std::move(task));
        }
        if (p.config.notify_all_on_enqueue) {
            p.wake_cv.notify_all();
        } else {
            p.wake_cv.notify_one();
        }
    }

} // namespace SFT::Async
