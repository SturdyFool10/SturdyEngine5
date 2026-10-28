#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <Async/IoHandler.hpp>
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

    template <class Rt, class F>
    [[nodiscard]] auto spawn_io(F &&fn) {
        if constexpr (requires { Rt::spawn_io(std::forward<F>(fn)); }) {
            return Rt::spawn_io(std::forward<F>(fn));
        } else {
            return Rt::spawn(std::forward<F>(fn));
        }
    }

    class ParallelRuntime {
      public:
        static constexpr bool is_parallel = true;

        /// Spawns the supplied asynchronous work.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn) {
            return Scheduler::spawn(std::forward<F>(fn));
        }

        template <typename F>
        [[nodiscard]] static auto spawn_io(F &&fn) {
            return IoHandler::submit(std::forward<F>(fn));
        }

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn, const Foundation::Cpu::IsaSet &required_isa,
                                        TaskWeight weight = TaskWeight::Light) {
            return Scheduler::spawn(std::forward<F>(fn), required_isa, weight);
        }

        /// Runs on main thread.
        ///
        /// @param fn Callable invoked by the operation.
        ///
        /// @note This function does not throw exceptions.
        static void run_on_main_thread(std::function<void()> fn) noexcept;

        /// Pumps main thread using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        static void pump_main_thread() noexcept;
    };

    class SynchronousRuntime {
      public:
        static constexpr bool is_parallel = false;

        /// Spawns the supplied asynchronous work.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename F>
        [[nodiscard]] static auto spawn_io(F &&fn) {
            return spawn(std::forward<F>(fn));
        }

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            Foundation::Cpu::IsaScope isa_scope(Foundation::Cpu::IsaSet{});
            if constexpr (std::is_void_v<R>) {
                std::forward<F>(fn)();
            } else {
                state->result.emplace(std::forward<F>(fn)());
            }
            state->mark_done();
            return TaskHandle<R>(std::move(state));
        }

        template <typename F>
        [[nodiscard]] static auto spawn(F &&fn, const Foundation::Cpu::IsaSet &required_isa) {
            using R = std::invoke_result_t<std::decay_t<F>>;
            auto state = std::make_shared<Detail::TaskState<R>>();
            if (!Foundation::Cpu::IsaScope::has_compatible_core(required_isa)) {
                state->isa_unavailable.store(true, std::memory_order_release);
                state->mark_done();
                return TaskHandle<R>(std::move(state));
            }
            Foundation::Cpu::IsaScope isa_scope(required_isa);
            if (!isa_scope.valid()) {
                state->isa_unavailable.store(true, std::memory_order_release);
                state->mark_done();
                return TaskHandle<R>(std::move(state));
            }
            if constexpr (std::is_void_v<R>) {
                std::forward<F>(fn)();
            } else {
                state->result.emplace(std::forward<F>(fn)());
            }
            state->mark_done();
            return TaskHandle<R>(std::move(state));
        }

        /// Runs on main thread.
        ///
        /// @param fn Callable invoked by the operation.
        ///
        /// @note This function does not throw exceptions.
        static void run_on_main_thread(std::function<void()> fn) noexcept;
        /// Pumps main thread using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        static void pump_main_thread() noexcept;
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
