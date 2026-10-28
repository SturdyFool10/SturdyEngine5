#pragma once

// Custom thread pools (Rayon's ThreadPoolBuilder / ThreadPool::install): a separate set of worker threads with its own
// size, for work that must not compete with -- or must be capped independently of -- the global scheduler (a background
// bake with 2 threads while the game keeps 6; an isolated pool per subsystem; deterministic thread counts in tests).
//
//   ThreadPool pool({.threads = 2, .name = "bake"});
//   long sum = pool.install([&] { return par_iter(data).map(f).sum(); });    // runs on exactly 2 threads
//
// "The current pool is the pool of the thread you are on": everything spawned or forked from inside `install` (join, scope,
// par_iter, sort, ...) stays in the pool, and code outside uses the global scheduler as usual.

#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace SFT::Async {

    struct ThreadPoolOptions {
        /// Worker threads; 0 means one per hardware thread.
        std::size_t threads = 0;
        /// Thread name prefix (visible in debuggers and profilers).
        std::string name = "pool";
    };

    namespace Detail {
        struct PoolImpl;
    }

    class ThreadPool {
      public:
        explicit ThreadPool(ThreadPoolOptions options = {});
        /// Finishes the queued work, then joins the workers. Must not be destroyed from one of its own threads.
        ~ThreadPool();
        ThreadPool(const ThreadPool &) = delete;
        ThreadPool &operator=(const ThreadPool &) = delete;
        ThreadPool(ThreadPool &&) noexcept;
        ThreadPool &operator=(ThreadPool &&) noexcept;

        [[nodiscard]] std::size_t num_threads() const noexcept;

        /// Runs `fn` on a pool thread and returns its result (rethrowing its exception). Parallel calls made inside use
        /// this pool. Called from one of the pool's own threads it simply runs `fn` inline.
        template <typename F>
        auto install(F &&fn) {
            if (on_pool_thread()) return std::forward<F>(fn)();
            return spawn(std::forward<F>(fn)).wait();
        }

        /// Starts `fn` on the pool and returns its handle.
        template <typename F>
        [[nodiscard]] auto spawn(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            submit(std::make_unique<Detail::ConcreteTask<std::decay_t<F>, R>>(std::forward<F>(fn), state));
            return TaskHandle<R>(std::move(state));
        }

        /// Whether the calling thread is one of this pool's workers.
        [[nodiscard]] bool on_pool_thread() const noexcept;

      private:
        void submit(std::unique_ptr<Detail::TaskBase> task);
        std::unique_ptr<Detail::PoolImpl> impl_;
    };

} // namespace SFT::Async
