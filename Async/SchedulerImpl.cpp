#include <Foundation/Foundation.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <Async/Scheduler.hpp>
#include <Async/Mutex.hpp>


using std::condition_variable;
using std::deque;
using std::lock_guard;
using std::make_unique;
using std::mt19937;
using std::random_device;
using std::thread;
using std::uniform_int_distribution;
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
            std::atomic<u32> pending_count{0}; // hint for idle workers; correctness never depends on it
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

        [[nodiscard]] unique_ptr<Detail::TaskBase> try_take_task(Pool &p, u32 index, mt19937 &rng) noexcept {
            unique_ptr<Detail::TaskBase> task = p.deques[index]->pop_back();
            if (!task) {
                task = p.injector.steal();
            }
            if (!task && p.deques.size() > 1) {
                const auto worker_total = static_cast<u32>(p.deques.size());
                uniform_int_distribution<u32> pick_victim(0, worker_total - 1);
                for (u32 attempt = 0; attempt < worker_total && !task; ++attempt) {
                    const u32 victim = pick_victim(rng);
                    if (victim != index) {
                        task = p.deques[victim]->steal();
                    }
                }
            }
            return task;
        }

        void worker_loop(u32 index) noexcept {
            t_worker_index = static_cast<i32>(index);
            Pool &p = pool();
            mt19937 rng(random_device{}());
            u32 idle_spins = 0;
            u32 idle_yields = 0;

            while (p.running.load(std::memory_order_acquire)) {
                unique_ptr<Detail::TaskBase> task = try_take_task(p, index, rng);

                if (task) {
                    idle_spins = 0;
                    idle_yields = 0;
                    task->execute();
                    p.pending_count.fetch_sub(1, std::memory_order_acq_rel);
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
                    return !p.running.load(std::memory_order_acquire) || p.pending_count.load(std::memory_order_acquire) > 0;
                });
                idle_spins = 0;
                idle_yields = 0;
            }
        }

    } // namespace

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
        p.pending_count.store(0, std::memory_order_release);
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

        if (t_worker_index >= 0) {
            p.deques[static_cast<usize>(t_worker_index)]->push_back(std::move(task));
        } else {
            p.injector.push_back(std::move(task));
        }

        p.pending_count.fetch_add(1, std::memory_order_acq_rel);
        if (p.config.notify_all_on_enqueue) {
            p.wake_cv.notify_all();
        } else {
            p.wake_cv.notify_one();
        }
    }

} // namespace SFT::Async
