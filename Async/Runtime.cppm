module;

#pragma region Imports
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#pragma endregion

export module Sturdy.Async:Runtime;

import :Task;
import :Scheduler;
import :MainThread;

export namespace SFT::Async {

    // Structural requirement for anything code can run tasks "within". `ParallelRuntime` (native: a
    // real work-stealing thread pool) and `SynchronousRuntime` (Web: no threads at all — everything
    // runs inline, on the calling thread, the instant it's "spawned") present this identical shape,
    // so higher-level systems can be written once against `AsyncRuntime` — typically via
    // `DefaultRuntime` below — and get real parallelism natively without a single `#ifdef`, while
    // still behaving correctly (serially) on Web.
    //
    // `is_parallel` is the one place that difference is visible: call sites that care whether
    // there's anything to gain from splitting work up (e.g. "chunk this loop into N pieces?") branch
    // on it via `if constexpr`; every other member behaves identically either way.
    // Checked with plain function pointers rather than lambda literals: a lambda's closure type
    // defined directly inside a requires-expression, instantiated here through the static_asserts
    // below, crashes this toolchain's name mangler (a closure has no ordinary linkage/mangling
    // context in that position). Function pointers exercise the exact same `spawn<F>` deduction
    // (any invocable, not just this one type) without hitting that.
    template <typename Rt>
    concept AsyncRuntime = requires(std::function<void()> job, void (*void_fn)(), int (*int_fn)()) {
        { Rt::spawn(void_fn).wait() } -> std::same_as<void>;
        { Rt::spawn(int_fn).wait() } -> std::same_as<int>;
        { Rt::run_on_main_thread(job) } -> std::same_as<void>;
        { Rt::pump_main_thread() } -> std::same_as<void>;
        { Rt::is_parallel } -> std::convertible_to<bool>;
    };

    // A callable usable as *parallel* work — dispatched to `Rt::spawn()` once per chunk/task (see
    // :Ranges, :ParIter) rather than called once, directly, the way `Foundation::SyncWork` (see
    // Foundation/Concepts.cppm) is. Two things can happen to a callable handed to the runtime, and
    // this concept has to cover both safely:
    //
    //  - it gets *copy-constructed*, once per task, so each task holds an independent instance (what
    //    `Ranges::for_each`/`Ranges::transform` do with the function you pass them directly) — needs
    //    `CopyConstructible`, not full `Copyable`: a reference-capturing lambda (very ordinary here —
    //    e.g. writing into an output vector captured by `&`) is copy-constructible but not
    //    copy-*assignable* (a reference member can't be reseated), and copy-assignment is never
    //    actually used anywhere this concept is checked;
    //  - it gets *shared*, folded into a `std::views::transform` chain that every task's iterator
    //    then reads through the same single stored instance (what `ParIter::map()`'s function
    //    becomes) — needs to be safely callable through a `const` reference, i.e. `operator()`
    //    doesn't mutate captured state (no `mutable` lambdas), so concurrent calls through that one
    //    shared instance never race.
    //
    // Requiring both up front means either dispatch strategy is safe without the caller having to
    // know or care which one a given method actually uses.
    template <class Fn, class... Args>
    concept AsyncWork = std::copy_constructible<std::remove_cvref_t<Fn>> && std::invocable<const std::remove_cvref_t<Fn> &, Args...>;

    // The native runtime: `spawn()` hands off to the work-stealing `Scheduler` (see :Scheduler),
    // main-thread jobs go through the :MainThread queue and only run once something calls
    // `pump_main_thread()` (see Engine::Application::run()).
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

    // The Web runtime: there are no worker threads to hand work to (a wasm build has no
    // Sturdy::Scheduler thread pool — see Core/CMakeLists.txt's Vulkan/Web split for the same shape
    // of platform gate elsewhere), so `spawn()` just runs `fn` immediately, synchronously, on the
    // calling thread, and hands back an already-completed `TaskHandle` — `wait()` returns at once
    // with the result, never blocking. `run_on_main_thread()` runs `fn` immediately too, since the
    // calling thread already *is* the only thread there is; `pump_main_thread()` is a no-op because
    // nothing is ever queued to begin with.
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

        static void run_on_main_thread(std::function<void()> fn) noexcept {
            fn();
        }

        static void pump_main_thread() noexcept {
        }
    };

    // The runtime this platform actually gets when code just wants "the" runtime rather than
    // naming one explicitly — ultra-fast (real parallelism) natively, correct-by-construction
    // (single-threaded, synchronous) on Web.
#if defined(STURDY_PLATFORM_WEB)
    using DefaultRuntime = SynchronousRuntime;
#else
    using DefaultRuntime = ParallelRuntime;
#endif

    static_assert(AsyncRuntime<ParallelRuntime>);
    static_assert(AsyncRuntime<SynchronousRuntime>);
    static_assert(AsyncRuntime<DefaultRuntime>);

} // namespace SFT::Async
