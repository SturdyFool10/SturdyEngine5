#include <Foundation/src/Foundation.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <Async/src/Affinity.hpp>
#include <Async/src/Topology.hpp>

using std::unique_lock;
using std::unique_ptr;

namespace SFT::Async {

    DedicatedThread::DedicatedThread(std::string name)
        : name_(std::move(name)) {
#if !defined(STURDY_PLATFORM_WEB)
        thread_ = std::thread(&DedicatedThread::worker_loop, this);
#endif
    }

    DedicatedThread::~DedicatedThread() noexcept {
        running_.store(false, std::memory_order_release);
        wake_cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void DedicatedThread::enqueue(unique_ptr<Detail::TaskBase> task) noexcept {
#if defined(STURDY_PLATFORM_WEB)
        // No background thread exists on Web (see the class docs) — run it immediately, inline.
        task->execute();
#else
        {
            auto guard = queue_.lock();
            guard->push_back(std::move(task));
        }
        wake_cv_.notify_one();
#endif
    }

    void DedicatedThread::worker_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            unique_ptr<Detail::TaskBase> task;
            {
                auto guard = queue_.lock();
                if (!guard->empty()) {
                    task = std::move(guard->front());
                    guard->pop_front();
                }
            }

            if (task) {
                task->execute();
                continue;
            }

            // Bounded poll rather than an indefinite wait: a missed notify_one() (the classic
            // condition_variable pitfall) then only costs up to this timeout, never correctness.
            unique_lock<std::mutex> lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
                if (!running_.load(std::memory_order_acquire)) {
                    return true;
                }
                auto guard = queue_.lock();
                return !guard->empty();
            });
        }
    }

    bool DedicatedThread::pin_to_core(u32 core_index) noexcept {
        return pin_thread_to_core(thread_, core_index);
    }

    bool DedicatedThread::pin_to_fastest_core() noexcept {
        // ranked_physical_cores(), not ranked_logical_cores(): pinning to a physical core rather than
        // a logical/SMT-sibling slot avoids landing this dedicated thread on the same physical core as
        // one of Async::Scheduler's own workers (SchedulerImpl.cpp sizes/pins its pool the same way).
        const std::vector<u32> ranked = ranked_physical_cores();
        if (ranked.empty()) {
            return false;
        }
        return pin_thread_to_core(thread_, ranked.front());
    }

} // namespace SFT::Async
