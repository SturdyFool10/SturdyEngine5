#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <Async/MainThread.hpp>
#include <Async/Scheduler.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    template <typename Rt>
    concept AsyncRuntime = requires(std::function<void()> job, void (*void_fn)(), int (*int_fn)()) {
        { Rt::spawn(void_fn).wait() } -> std::same_as<void>;
        { Rt::spawn(int_fn).wait() } -> std::same_as<int>;
        { Rt::run_on_main_thread(job) } -> std::same_as<void>;
        { Rt::pump_main_thread() } -> std::same_as<void>;
        { Rt::is_parallel } -> std::convertible_to<bool>;
    };

    template <class Fn, class... Args>
    concept AsyncWork = std::copy_constructible<std::remove_cvref_t<Fn>> &&
        std::invocable<const std::remove_cvref_t<Fn> &, Args...>;

    class ParallelRuntime {
      public:
        static constexpr bool is_parallel = true;

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn) {
            return Scheduler::spawn(std::forward<F>(fn));
        }

        static void run_on_main_thread(std::function<void()> fn) noexcept {
            SFT::Async::run_on_main_thread(std::move(fn));
        }

        static void pump_main_thread() noexcept {
            SFT::Async::pump_main_thread();
        }
    };

    class SynchronousRuntime {
      public:
        static constexpr bool is_parallel = false;

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            if constexpr (std::is_void_v<R>) {
                std::forward<F>(fn)();
            } else {
                state->result.emplace(std::forward<F>(fn)());
            }
            state->mark_done();
            return TaskHandle<R>(std::move(state));
        }

        static void run_on_main_thread(std::function<void()> fn) noexcept { fn(); }
        static void pump_main_thread() noexcept {}
    };

#if defined(STURDY_PLATFORM_WEB)
    using DefaultRuntime = SynchronousRuntime;
#else
    using DefaultRuntime = ParallelRuntime;
#endif

    static_assert(AsyncRuntime<ParallelRuntime>);
    static_assert(AsyncRuntime<SynchronousRuntime>);
    static_assert(AsyncRuntime<DefaultRuntime>);

} // namespace SFT::Async
