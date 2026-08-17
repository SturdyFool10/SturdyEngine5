#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <Async/src/Mutex.hpp>
#include <Async/src/Scheduler.hpp>
#include <Async/src/Task.hpp>

namespace SFT::Async {

    class DedicatedThread {
      public:
        /// Constructs a `DedicatedThread` from the supplied initialization values.
        ///
        /// @param name Name used to identify or label the target.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit DedicatedThread(std::string name = "DedicatedThread");
        /// Destroys the `DedicatedThread` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~DedicatedThread() noexcept;

        /// Disables this construction form for `DedicatedThread`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        DedicatedThread(const DedicatedThread &) = delete;
        /// Assigns a new value to this `DedicatedThread`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        DedicatedThread &operator=(const DedicatedThread &) = delete;
        /// Disables this construction form for `DedicatedThread`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        DedicatedThread(DedicatedThread &&) = delete;
        /// Assigns a new value to this `DedicatedThread`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        DedicatedThread &operator=(DedicatedThread &&) = delete;

        /// Runs the requested work.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        [[nodiscard]] auto run(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto task = std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task));
            return TaskHandle<R>(std::move(state));
        }

        /// Performs the pin to core operation for `DedicatedThread` using the supplied arguments.
        ///
        /// @param core_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pin_to_core(u32 core_index) noexcept;


        /// Returns the current or globally available pin to fastest core value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pin_to_fastest_core() noexcept;

        /// Returns the current or globally available name value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::string &name() const noexcept;

      private:
        /// Performs the enqueue operation for `DedicatedThread` using the supplied arguments.
        ///
        /// @param task Task used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void enqueue(std::unique_ptr<Detail::TaskBase> task) noexcept;
        /// Performs the worker loop operation for `DedicatedThread` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void worker_loop() noexcept;

        std::string name_;
        Mutex<std::deque<std::unique_ptr<Detail::TaskBase>>> queue_;
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;
        std::atomic<bool> running_{true};
        std::thread thread_;
    };

} // namespace SFT::Async
