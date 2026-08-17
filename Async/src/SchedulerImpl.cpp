#include <Foundation/src/Foundation.hpp>
#include <Foundation/src/Cpu/CoreMap.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <Async/src/Scheduler.hpp>
#include <Async/src/Mutex.hpp>
#include <Async/src/Topology.hpp>

#include <tracy/Tracy.hpp>


using std::condition_variable;
using std::deque;
using std::make_unique;
using std::thread;
using std::unique_lock;
using std::unique_ptr;
using std::vector;

namespace SFT::Async {

    namespace {

        [[nodiscard]] const char *core_type_name(Foundation::Cpu::CoreType type) noexcept {
            switch (type) {
                case Foundation::Cpu::CoreType::Performance: return "Performance";
                case Foundation::Cpu::CoreType::Efficiency: return "Efficiency";
                case Foundation::Cpu::CoreType::Unknown: return "Unknown";
            }
            return "?";
        }





        class WorkerDeque {
          public:




            explicit WorkerDeque(const char *debug_name = nullptr) noexcept { tasks_.set_debug_name(debug_name); }

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
            WorkerDeque injector{"Scheduler Injector"};
            std::atomic<bool> running{false};


            std::atomic<u32> queued_count{0};
            std::atomic<u32> waiting_worker_count{0};
            std::mutex wake_mutex;
            condition_variable wake_cv;
            SchedulerConfig config{};






            usize heavy_worker_count = 1;
            std::atomic<u32> heavy_round_robin{0};
        };

        Pool &pool() noexcept {
            static Pool instance;
            return instance;
        }


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













            if (p.queued_count.load(std::memory_order_acquire) == 0) {
                return false;
            }

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


            char tracy_thread_name[32];
            std::snprintf(tracy_thread_name, sizeof(tracy_thread_name), "Async Worker %u", index);
            tracy::SetThreadName(tracy_thread_name);
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
            const vector<u32> physical_cores = ranked_physical_cores();
            const u32 physical_count = physical_cores.empty()
                ? std::max<u32>(1, thread::hardware_concurrency())
                : static_cast<u32>(physical_cores.size());
            active_config.worker_count = physical_count > 1 ? physical_count - 1 : 1;
        }
        if (active_config.idle_sleep_microseconds == 0) {
            active_config.idle_sleep_microseconds = 1;
        }
        p.config = active_config;

        const u32 worker_count = active_config.worker_count;
        p.deques.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            char deque_name[32];
            std::snprintf(deque_name, sizeof(deque_name), "Scheduler Worker %u Deque", i);
            p.deques.push_back(make_unique<WorkerDeque>(deque_name));
        }

        p.threads.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            p.threads.emplace_back(worker_loop, i);
        }









        const vector<u32> ranked_cores = ranked_physical_cores();
        u32 pinned_count = 0;
        if (!ranked_cores.empty()) {
            for (u32 i = 0; i < worker_count; ++i) {
                if (pin_thread_to_core(p.threads[i], ranked_cores[i % ranked_cores.size()])) {
                    ++pinned_count;
                }
            }
        }
        p.heavy_worker_count = std::max<usize>(1, worker_count / 4);

        const auto &core_map = Foundation::Cpu::CoreMap::instance();
        Foundation::log_info(
            "Async::Scheduler started {} worker thread(s) [spin={}, yield={}, sleep={}us], "
            "{} pinned, {} heavy-preferred; topology: {} logical core(s), {} physical core(s), "
            "{} distinct type(s), hybrid={}.",
            worker_count,
            active_config.idle_spin_iterations,
            active_config.idle_yield_iterations,
            active_config.idle_sleep_microseconds,
            pinned_count,
            p.heavy_worker_count,
            core_map.core_count(),
            core_map.physical_core_count(),
            core_map.distinct_type_count(),
            core_map.is_hybrid());




        for (usize type_index = 0; type_index < core_map.distinct_type_count(); ++type_index) {
            const vector<usize> &members = core_map.core_indices_of_type(type_index);
            const Foundation::Cpu::CoreCapabilities &rep = core_map.core(members.front());

            usize extension_count = 0;
            for (const bool bit : rep.extensions) {
                extension_count += bit ? 1 : 0;
            }

            Foundation::log_info(
                "  topology type[{}]: {} core(s), core_type={}, {} extension(s), "
                "l1d={}B l1i={}B l2={}B l3={}B",
                type_index,
                members.size(),
                core_type_name(rep.type),
                extension_count,
                rep.l1d_bytes,
                rep.l1i_bytes,
                rep.l2_bytes,
                rep.l3_bytes);
        }
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

    void Scheduler::enqueue(unique_ptr<Detail::TaskBase> task, TaskWeight weight) noexcept {
        Pool &p = pool();
        if (!p.running.load(std::memory_order_acquire)) {
            initialize();
        }





        p.queued_count.fetch_add(1, std::memory_order_acq_rel);





        if (weight == TaskWeight::Heavy && p.heavy_worker_count > 0) {
            const u32 index = p.heavy_round_robin.fetch_add(1, std::memory_order_relaxed) %
                               static_cast<u32>(p.heavy_worker_count);
            p.deques[index]->push_back(std::move(task));
        } else if (t_worker_index >= 0) {
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
