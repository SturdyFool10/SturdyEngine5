#pragma once

#include <Async/Cancellation.hpp>
#include <Async/Task.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace SFT::Async {

    enum class IoBackend {
        BlockingThreadPool,
        Synchronous,
    };

    struct IoHandlerConfig {
        /// Zero selects an automatic worker count (clamped to [2, 8]).
        unsigned worker_count = 0;
        /// Maximum operations waiting for a worker. Active operations are not counted.
        unsigned max_pending_operations = 1024;
    };

    class IoQueueFull final : public std::runtime_error {
      public:
        IoQueueFull() : std::runtime_error("Async IO queue is full") {}
    };

    /// Dedicated execution lane for blocking or latency-bound IO.
    ///
    /// Operations run independently from the CPU work-stealing scheduler. Native async backends
    /// can be selected behind this facade; unsupported work uses the bounded blocking pool.
    class IoHandler {
      public:
        IoHandler() = delete;

        /// Configures the fallback pool. Must be called before the first submission or pool query.
        /// Returns false for invalid limits or if the handler has already started.
        [[nodiscard]] static bool configure(const IoHandlerConfig &config);

        /// Submits one operation to the IO lane.
        /// @throws IoQueueFull immediately if the pending queue has reached its configured capacity.
        /// Exceptions from the operation are rethrown by the returned handle's wait().
        template <typename F>
        [[nodiscard]] static auto submit(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            auto operation = [callable = std::decay_t<F>(std::forward<F>(fn)), state, context = Detail::t_context]() mutable {
                Detail::ContextScope context_scope(context);
                try {
                    Foundation::Cpu::IsaScope scalar_scope(Foundation::Cpu::IsaSet{});
                    if constexpr (std::is_void_v<R>) {
                        callable();
                    } else {
                        state->result.emplace(callable());
                    }
                } catch (...) {
                    state->exception = std::current_exception();
                }
                state->mark_done();
            };
            if (is_worker_thread()) {
                operation();
            } else {
                enqueue(std::move(operation));
            }
            return TaskHandle<R>(std::move(state));
        }

        /// Submits an operation with a cancellation token. Queued operations cancelled before
        /// execution are skipped. Running callbacks may accept the token and poll it cooperatively.
        template <typename F>
        [[nodiscard]] static auto submit(F &&fn, CancellationToken token) {
            using Fn = std::decay_t<F>;
            return submit([callable = Fn(std::forward<F>(fn)), token]() mutable -> decltype(auto) {
                token.throw_if_cancelled();
                if constexpr (std::is_invocable_v<Fn &, CancellationToken>) {
                    return std::invoke(callable, token);
                } else {
                    return std::invoke(callable);
                }
            });
        }

        /// Returns the active backend. Native backends are added without changing callers.
        [[nodiscard]] static IoBackend backend() noexcept;
        /// Returns whether the caller is currently executing on an IO worker.
        [[nodiscard]] static bool is_worker_thread() noexcept;
        /// Returns the number of threads in the blocking fallback pool.
        [[nodiscard]] static unsigned worker_count();
        /// Returns the number of waiting operations.
        [[nodiscard]] static unsigned pending_count();
        /// Returns the configured maximum number of waiting operations.
        [[nodiscard]] static unsigned pending_capacity();
        /// Stops accepting work and waits for accepted operations to drain. Returns false when
        /// called from an IO worker, where joining the pool would deadlock. A blocking OS call
        /// may delay shutdown until that operation returns.
        [[nodiscard]] static bool shutdown();

      private:
        static void enqueue(std::function<void()> operation);
    };

} // namespace SFT::Async
