#pragma once

// Fork-join primitives (the Rayon half of Async): join, scope, recursive range splitting with a deterministic
// leaf partition, and parallel sorts. Everything is generic over the runtime `Rt`; on a runtime that is not
// parallel (e.g. the synchronous Web runtime) every call degrades to plain sequential code with the same results.
//
// Blocking inside these calls is cheap and safe: `TaskHandle::wait()` on a scheduler worker executes other queued
// tasks instead of idling, so nesting parallel calls inside parallel calls does not deadlock or oversubscribe.

#include <Async/Runtime.hpp>
#include <Async/Task.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Async {

    /// Stand-in for `void` where a result must be stored (join halves, per-leaf results).
    struct Unit {};

    namespace Detail {
        template <typename T>
        using ValueOrUnit = std::conditional_t<std::is_void_v<T>, Unit, T>;

        template <typename F>
        decltype(auto) invoke_unit(F &&fn) {
            if constexpr (std::is_void_v<std::invoke_result_t<F &>>) {
                std::invoke(fn);
                return Unit{};
            } else {
                return std::invoke(fn);
            }
        }
    } // namespace Detail

    /// Threads that parallel calls on `Rt` can use (1 for non-parallel runtimes).
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] inline usize current_num_threads() noexcept {
        if constexpr (!Rt::is_parallel) {
            return 1;
        } else {
            if (Detail::t_pool != nullptr) return static_cast<usize>(Detail::t_pool->thread_count()); // inside a user pool
            usize workers = 0;
            if constexpr (std::same_as<Rt, ParallelRuntime>) workers = Scheduler::worker_count();
            return workers != 0 ? workers : (std::max)(usize{1}, static_cast<usize>(std::thread::hardware_concurrency()));
        }
    }

    /// Runs `a` and `b` potentially in parallel and returns both results (`Unit` for void). `a` runs on the calling
    /// thread, `b` is offered to other workers (and run by the caller if nobody takes it). If either throws, the
    /// other still finishes first -- both may reference the caller's stack -- and the exception propagates.
    template <AsyncRuntime Rt = DefaultRuntime, typename A, typename B>
    [[nodiscard]] auto join(A &&a, B &&b) {
        using RA = Detail::ValueOrUnit<std::invoke_result_t<A &>>;
        using RB = Detail::ValueOrUnit<std::invoke_result_t<B &>>;
        auto handle_b = Rt::spawn([&b]() -> RB { return Detail::invoke_unit(b); });
        std::optional<RA> result_a;
        try {
            result_a.emplace(Detail::invoke_unit(a));
        } catch (...) {
            try {
                handle_b.wait();
            } catch (...) { // `a`'s exception wins; `b` has finished, which is what matters
            }
            throw;
        }
        RB result_b = handle_b.wait();
        return std::pair<RA, RB>(std::move(*result_a), std::move(result_b));
    }

    /// A set of tasks that may borrow from the enclosing stack frame; `scope` does not return until all finished.
    template <AsyncRuntime Rt = DefaultRuntime>
    class Scope {
      public:
        /// Starts `fn` on the runtime. `fn` may call `spawn` again (the scope outlives it). Exceptions are held
        /// and the first one is rethrown by `scope` after everything finished.
        template <typename F>
        void spawn(F &&fn) {
            auto handle = Rt::spawn([fn = std::decay_t<F>(std::forward<F>(fn))]() mutable { fn(); });
            auto guard = pending_.lock();
            guard->push_back(std::move(handle));
        }

        /// Internal: used by `scope`.
        void wait_all() {
            for (;;) {
                std::vector<TaskHandle<void>> batch;
                {
                    auto guard = pending_.lock();
                    batch.swap(*guard);
                }
                if (batch.empty()) return;
                for (auto &handle : batch) {
                    try {
                        handle.wait();
                    } catch (...) {
                        auto guard = errors_.lock();
                        if (!*guard) *guard = std::current_exception();
                    }
                }
            }
        }

        Mutex<std::exception_ptr> errors_;

      private:
        Mutex<std::vector<TaskHandle<void>>> pending_;
    };

    /// Runs `body(Scope&)`, then waits for every task spawned on the scope, then rethrows the first exception
    /// raised by `body` or by any task.
    template <AsyncRuntime Rt = DefaultRuntime, typename Body>
    auto scope(Body &&body) {
        Scope<Rt> s;
        std::exception_ptr body_error;
        using R = Detail::ValueOrUnit<std::invoke_result_t<Body &, Scope<Rt> &>>;
        std::optional<R> result;
        try {
            result.emplace([&]() -> R {
                if constexpr (std::is_void_v<std::invoke_result_t<Body &, Scope<Rt> &>>) {
                    body(s);
                    return Unit{};
                } else {
                    return body(s);
                }
            }());
        } catch (...) {
            body_error = std::current_exception();
        }
        s.wait_all();
        if (body_error) std::rethrow_exception(body_error);
        if (auto guard = s.errors_.lock(); *guard) std::rethrow_exception(*guard);
        if constexpr (!std::is_void_v<std::invoke_result_t<Body &, Scope<Rt> &>>) return std::move(*result);
    }

    namespace Detail {

        /// Cooperative cancellation for parallel loops: set by a leaf that found its answer or failed.
        struct StopFlag {
            std::atomic<bool> value{false};
            [[nodiscard]] bool stopped() const noexcept { return value.load(std::memory_order_relaxed); }
            void stop() noexcept { value.store(true, std::memory_order_relaxed); }
        };

        struct LeafPlan {
            usize leaf_len;
            usize leaf_count;
        };

        /// Splits `n` items into about 8 leaves per thread (so stealing can even out uneven work), never below
        /// `min_len` items per leaf and never above `max_len`.
        template <AsyncRuntime Rt>
        [[nodiscard]] inline LeafPlan plan_leaves(usize n, usize min_len, usize max_len) noexcept {
            if (n == 0) return {1, 0};
            const usize target = (std::max)(usize{1}, current_num_threads<Rt>() * 8);
            usize len = (n + target - 1) / target;
            len = (std::max)(len, (std::max)(min_len, usize{1}));
            len = (std::min)(len, (std::max)(max_len, usize{1}));
            return {len, (n + len - 1) / len};
        }

        template <AsyncRuntime Rt, typename Body>
        struct LeafRecursion {
            Body &body;
            usize n;
            usize leaf_len;

            void run(usize lo, usize hi) {
                if (hi - lo == 1) {
                    body(lo * leaf_len, (std::min)(n, (lo + 1) * leaf_len), lo);
                    return;
                }
                const usize mid = lo + (hi - lo) / 2;
                (void)join<Rt>([&] { run(lo, mid); }, [&] { run(mid, hi); });
            }
        };

        /// Calls `leaf(begin, end, leaf_index)` for every leaf of the plan -- potentially in parallel, the caller
        /// taking part -- and returns the leaf results in leaf order. A throwing leaf stops the others early
        /// (they check `stop`) and its exception propagates after all leaves finished.
        template <AsyncRuntime Rt, typename Leaf>
        [[nodiscard]] auto run_leaves(usize n, usize min_len, usize max_len, StopFlag &stop, Leaf &&leaf) {
            using R = ValueOrUnit<std::invoke_result_t<Leaf &, usize, usize, usize>>;
            const LeafPlan plan = plan_leaves<Rt>(n, min_len, max_len);
            std::vector<std::optional<R>> slots(plan.leaf_count);
            if (plan.leaf_count == 0) return slots;
            auto body = [&](usize begin, usize end, usize index) {
                try {
                    slots[index].emplace(invoke_unit([&] { return leaf(begin, end, index); }));
                } catch (...) {
                    stop.stop();
                    throw;
                }
            };
            LeafRecursion<Rt, decltype(body)> recursion{body, n, plan.leaf_len};
            recursion.run(0, plan.leaf_count);
            return slots;
        }

        // ---- parallel merge sort -----------------------------------------------------------------
        constexpr usize kSortLeaf = 4096;
        constexpr usize kMergeLeaf = 8192;

        /// Merges sorted [a0,a1) and [b0,b1) into `out`, splitting the larger run at its midpoint and binary
        /// searching the other so both halves can proceed in parallel. Ties take elements of `a` first (stable).
        template <AsyncRuntime Rt, typename It, typename Out, typename Cmp>
        void par_merge(It a0, It a1, It b0, It b1, Out out, const Cmp &cmp) {
            const auto na = a1 - a0;
            const auto nb = b1 - b0;
            if (static_cast<usize>(na + nb) <= kMergeLeaf) {
                // Hand-written so the comparator always sees lvalues, exactly as with std::sort.
                while (a0 != a1 && b0 != b1) {
                    if (cmp(*b0, *a0)) *out++ = std::move(*b0++);
                    else *out++ = std::move(*a0++);
                }
                out = std::move(a0, a1, out);
                std::move(b0, b1, out);
                return;
            }
            It am, bm;
            if (na >= nb) {
                am = a0 + na / 2;
                bm = std::lower_bound(b0, b1, *am, cmp);
            } else {
                bm = b0 + nb / 2;
                am = std::upper_bound(a0, a1, *bm, cmp);
            }
            const auto left_count = (am - a0) + (bm - b0);
            (void)join<Rt>([&] { par_merge<Rt>(a0, am, b0, bm, out, cmp); },
                           [&] { par_merge<Rt>(am, a1, bm, b1, out + left_count, cmp); });
        }

        template <AsyncRuntime Rt, typename It, typename Buf, typename Cmp>
        void merge_sort(It first, It last, Buf buffer, const Cmp &cmp, bool stable_leaves) {
            const usize n = static_cast<usize>(last - first);
            if (n <= kSortLeaf) {
                if (stable_leaves) std::stable_sort(first, last, cmp);
                else std::sort(first, last, cmp);
                return;
            }
            const It mid = first + static_cast<std::ptrdiff_t>(n / 2);
            const auto half = static_cast<std::ptrdiff_t>(n / 2);
            (void)join<Rt>([&] { merge_sort<Rt>(first, mid, buffer, cmp, stable_leaves); },
                           [&] { merge_sort<Rt>(mid, last, buffer + half, cmp, stable_leaves); });
            par_merge<Rt>(first, mid, mid, last, buffer, cmp);
            // Move the merged run back, in parallel for big ranges.
            struct Copy {
                static void run(Buf from, It to, usize count) {
                    if (count <= kMergeLeaf) {
                        std::move(from, from + static_cast<std::ptrdiff_t>(count), to);
                        return;
                    }
                    const usize h = count / 2;
                    (void)join<Rt>([&] { run(from, to, h); }, [&] { run(from + static_cast<std::ptrdiff_t>(h), to + static_cast<std::ptrdiff_t>(h), count - h); });
                }
            };
            Copy::run(buffer, first, n);
        }

    } // namespace Detail

    /// Stable parallel merge sort (equal elements keep their order). Needs `T` default-constructible and movable;
    /// uses n extra elements of scratch space. Below a few thousand elements it is just `std::stable_sort`.
    template <AsyncRuntime Rt = DefaultRuntime, std::random_access_iterator It, typename Cmp = std::less<>>
    void par_sort(It first, It last, Cmp cmp = {}) {
        const usize n = static_cast<usize>(last - first);
        if (n <= Detail::kSortLeaf || !Rt::is_parallel) {
            std::stable_sort(first, last, cmp);
            return;
        }
        std::vector<std::iter_value_t<It>> buffer(n);
        Detail::merge_sort<Rt>(first, last, buffer.begin(), cmp, true);
    }

    /// Parallel sort without the stability guarantee (faster leaves).
    template <AsyncRuntime Rt = DefaultRuntime, std::random_access_iterator It, typename Cmp = std::less<>>
    void par_sort_unstable(It first, It last, Cmp cmp = {}) {
        const usize n = static_cast<usize>(last - first);
        if (n <= Detail::kSortLeaf || !Rt::is_parallel) {
            std::sort(first, last, cmp);
            return;
        }
        std::vector<std::iter_value_t<It>> buffer(n);
        Detail::merge_sort<Rt>(first, last, buffer.begin(), cmp, false);
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::random_access_iterator It, typename Key>
    void par_sort_by_key(It first, It last, Key key) {
        par_sort<Rt>(first, last, [&key](const auto &a, const auto &b) { return key(a) < key(b); });
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Cmp = std::less<>>
    void par_sort(R &&range, Cmp cmp = {}) {
        par_sort<Rt>(std::ranges::begin(range), std::ranges::end(range), std::move(cmp));
    }

    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::random_access_range R, typename Cmp = std::less<>>
    void par_sort_unstable(R &&range, Cmp cmp = {}) {
        par_sort_unstable<Rt>(std::ranges::begin(range), std::ranges::end(range), std::move(cmp));
    }

} // namespace SFT::Async
