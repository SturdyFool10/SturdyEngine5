module;

#pragma region Imports
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#pragma endregion

export module Sturdy.Async:Affinity;

import Sturdy.Foundation;
import :Task;
import :Mutex;

export namespace SFT::Async {

    // A single dedicated background thread with its own strictly-serial FIFO task queue: `run()`
    // may be called from anywhere, but everything handed to *this* `DedicatedThread` executes one
    // at a time, in submission order, on the one thread it owns — never concurrently, never on any
    // other thread. That is the actual guarantee the Vulkan spec asks for around `vkQueueSubmit` /
    // `vkQueueBindSparse` / `vkQueuePresentKHR` on a given `VkQueue` ("must be externally
    // synchronized" — no two threads touching the same queue at once); it is *not* a same-core
    // requirement, so this class does not pin to a core unless you explicitly ask it to with
    // `pin_to_core()`.
    //
    // On Web there is no real thread here at all (no worker threads exist for Web builds — see
    // :Runtime's SynchronousRuntime) — `run()` simply executes `fn` immediately, inline, which
    // still satisfies "everything runs one at a time on one thread" trivially, since there is only
    // ever the one thread to begin with.
    class DedicatedThread {
      public:
        explicit DedicatedThread(std::string name = "DedicatedThread");
        ~DedicatedThread() noexcept;

        DedicatedThread(const DedicatedThread &) = delete;
        DedicatedThread &operator=(const DedicatedThread &) = delete;
        DedicatedThread(DedicatedThread &&) = delete;
        DedicatedThread &operator=(DedicatedThread &&) = delete;

        // Schedules `fn` to run on this thread and returns a handle to observe/await it. Safe to
        // call from any thread, including this one (it lands at the back of the same queue, so a
        // task already running here can enqueue follow-up work for itself without deadlocking).
        template <typename F>
        [[nodiscard]] auto run(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto task = std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state);
            enqueue(std::move(task));
            return TaskHandle<R>(std::move(state));
        }

        // Best-effort hardware affinity: pins this thread to CPU core `core_index`. Returns whether
        // the OS actually accepted it — always `false` on Web (there is no OS thread to pin) and,
        // on macOS, Apple's Mach thread-affinity-tag API is a *scheduling hint*, not a hard
        // guarantee, unlike Linux/FreeBSD (`pthread_setaffinity_np`) and Windows
        // (`SetThreadAffinityMask`), which do bind exactly.
        [[nodiscard]] bool pin_to_core(u32 core_index) noexcept;

        [[nodiscard]] const std::string &name() const noexcept {
            return name_;
        }

      private:
        void enqueue(std::unique_ptr<Detail::TaskBase> task) noexcept;
        void worker_loop() noexcept;

        std::string name_;
        Mutex<std::deque<std::unique_ptr<Detail::TaskBase>>> queue_;
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;
        std::atomic<bool> running_{true};
        std::thread thread_; // not-a-thread (joinable() == false) on Web — see class docs.
    };

} // namespace SFT::Async
