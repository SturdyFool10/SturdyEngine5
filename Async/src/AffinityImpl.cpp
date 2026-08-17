#include <Foundation/src/Foundation.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <Async/src/Affinity.hpp>
#include <Async/src/Topology.hpp>

#include <tracy/Tracy.hpp>

using std::unique_lock;
using std::unique_ptr;

namespace SFT::Async {

    /// Performs the dedicated thread operation for `Async` using the supplied arguments.
    ///
    /// @param name Name used to identify or label the target.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DedicatedThread::DedicatedThread(std::string name)
        : name_(std::move(name)) {
#if !defined(STURDY_PLATFORM_WEB)
        thread_ = std::thread(&DedicatedThread::worker_loop, this);
#endif
    }

    /// Destroys the `Async` and releases resources owned by it.
    ///
    /// @note This function does not throw exceptions.
    DedicatedThread::~DedicatedThread() noexcept {
        running_.store(false, std::memory_order_release);
        wake_cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// Performs the enqueue operation for `Async` using the supplied arguments.
    ///
    /// @param task Task used or affected by the operation.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function does not throw exceptions.
    void DedicatedThread::enqueue(unique_ptr<Detail::TaskBase> task) noexcept {
#if defined(STURDY_PLATFORM_WEB)

        task->execute();
#else
        {
            auto guard = queue_.lock();
            guard->push_back(std::move(task));
        }
        wake_cv_.notify_one();
#endif
    }

    /// Returns the current or globally available worker loop value.
    ///
    /// @return Returns the current worker loop value.
    /// @note This function does not throw exceptions.
    void DedicatedThread::worker_loop() noexcept {


        tracy::SetThreadName(name_.c_str());
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

    /// Performs the pin to core operation for `Async` using the supplied arguments.
    ///
    /// @param core_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool DedicatedThread::pin_to_core(u32 core_index) noexcept {
        return pin_thread_to_core(thread_, core_index);
    }

    /// Returns the current or globally available pin to fastest core value.
    ///
    /// @return Returns the current pin to fastest core value.
    /// @note This function does not throw exceptions.
    bool DedicatedThread::pin_to_fastest_core() noexcept {


        const std::vector<u32> ranked = ranked_physical_cores();
        if (ranked.empty()) {
            return false;
        }
        return pin_thread_to_core(thread_, ranked.front());
    }

} // namespace SFT::Async
