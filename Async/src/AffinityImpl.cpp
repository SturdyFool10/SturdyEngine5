#include <Foundation/src/Foundation.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if !defined(STURDY_PLATFORM_WEB)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#endif
#include <Async/src/Affinity.hpp>

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
#if defined(STURDY_PLATFORM_WEB)
        (void)core_index;
        return false;
#elif defined(_WIN32)
        if (!thread_.joinable()) {
            return false;
        }
        const DWORD_PTR mask = DWORD_PTR{1} << core_index;
        return SetThreadAffinityMask(thread_.native_handle(), mask) != 0;
#elif defined(__APPLE__)
        // Mach's thread-affinity-tag API is a *scheduling hint* for cache/locality grouping, not a
        // hard core pin — macOS offers no equivalent of Linux/Windows' exact affinity masks.
        if (!thread_.joinable()) {
            return false;
        }
        thread_affinity_policy_data_t policy{.affinity_tag = static_cast<integer_t>(core_index)};
        const mach_port_t mach_thread = pthread_mach_thread_np(thread_.native_handle());
        return thread_policy_set(mach_thread,
                                  THREAD_AFFINITY_POLICY,
                                  reinterpret_cast<thread_policy_t>(&policy),
                                  THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#elif defined(__linux__)
        if (!thread_.joinable()) {
            return false;
        }
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(static_cast<int>(core_index), &cpu_set);
        return pthread_setaffinity_np(thread_.native_handle(), sizeof(cpu_set_t), &cpu_set) == 0;
#else
        // FreeBSD (and anything else): not implemented yet — FreeBSD's cpuset_t API lives under
        // <pthread_np.h>/<sys/cpuset.h> and differs enough from Linux's that it needs its own
        // verified implementation rather than a guess. Flesh out when there's a FreeBSD box to test
        // against (same stance Platform/CMakeLists.txt already takes for FreeBSD-specific code).
        (void)core_index;
        return false;
#endif
    }

} // namespace SFT::Async
