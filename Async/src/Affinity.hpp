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
        explicit DedicatedThread(std::string name = "DedicatedThread");
        ~DedicatedThread() noexcept;

        DedicatedThread(const DedicatedThread &) = delete;
        DedicatedThread &operator=(const DedicatedThread &) = delete;
        DedicatedThread(DedicatedThread &&) = delete;
        DedicatedThread &operator=(DedicatedThread &&) = delete;

        template <typename F>
        [[nodiscard]] auto run(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto task = std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task));
            return TaskHandle<R>(std::move(state));
        }

        [[nodiscard]] bool pin_to_core(u32 core_index) noexcept;
        [[nodiscard]] const std::string &name() const noexcept { return name_; }

      private:
        void enqueue(std::unique_ptr<Detail::TaskBase> task) noexcept;
        void worker_loop() noexcept;

        std::string name_;
        Mutex<std::deque<std::unique_ptr<Detail::TaskBase>>> queue_;
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;
        std::atomic<bool> running_{true};
        std::thread thread_;
    };

} // namespace SFT::Async
