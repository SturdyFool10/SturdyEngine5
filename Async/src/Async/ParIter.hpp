#pragma once

// Data-parallel iterators (the Rayon half of Async).
//
//   Async::par_iter(v).map(f).filter(p).sum()        Async::par_iter(v) | ...        Async::par_iter_on<Rt>(v)
//
// Two kinds of iterator, like Rayon's indexed / unindexed split:
//   * ParIter  -- random access: knows its length and element i, so it supports zip, enumerate, rev, take, skip,
//                 step_by, chain, chunks/windows and collects straight into pre-sized storage.
//   * ParFlow  -- what `filter` / `filter_map` / `flat_map` turn it into: a base ParIter plus a per-element
//                 pipeline. No indices, but ordered collect, reduce, count, find_first and short-circuiting all work.
//
// Work is split into ~8 leaves per thread and balanced by work stealing. Results are always combined in leaf order,
// so for a given thread count `reduce`/`sum`/`collect` are deterministic; non-associative operations (floating-point
// sums) may differ between thread counts, exactly as in Rayon. Closures run concurrently and must be const-callable
// and thread-safe; use `for_each_init` / `map_init` for per-task scratch state.

#include <Async/Chunk.hpp>
#include <Async/Parallel.hpp>
#include <Async/Ranges.hpp>
#include <Async/Runtime.hpp>
#include <Async/Task.hpp>
#include <Foundation/ParIter.hpp>
#include <Foundation/RangesCompat.hpp>

#include <algorithm>
#include <concepts>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace SFT::Async {

    namespace Detail {
        /// Reduction kernels for contiguous arithmetic data. IEEE addition is not associative, so the compiler may not
        /// vectorise the naive loop for floating point; independent accumulators make the lanes explicit (about 10x faster on
        /// AVX2/AVX-512 in measurements). The summation order is fixed by the data size alone, so results are repeatable.
        /// Integers are left to the compiler, which already vectorises them.
        template <typename T>
        [[nodiscard]] inline T sum_span(std::span<const T> values) {
            if constexpr (std::is_floating_point_v<T>) {
                constexpr std::size_t lanes = 16;
                T acc[lanes] = {};
                std::size_t i = 0;
                const std::size_t n = values.size();
                for (; i + lanes <= n; i += lanes)
                    for (std::size_t k = 0; k < lanes; ++k) acc[k] += values[i + k];
                T total{};
                for (std::size_t k = 0; k < lanes; ++k) total += acc[k];
                for (; i < n; ++i) total += values[i];
                return total;
            } else {
                T total{};
                for (const T x : values) total += x;
                return total;
            }
        }

        /// First minimum / last maximum semantics match the generic path; `values` must not be empty.
        template <typename T>
        [[nodiscard]] inline T min_span(std::span<const T> values) {
            if constexpr (std::is_floating_point_v<T>) {
                constexpr std::size_t lanes = 16;
                T acc[lanes];
                for (std::size_t k = 0; k < lanes; ++k) acc[k] = values[0];
                std::size_t i = 0;
                const std::size_t n = values.size();
                for (; i + lanes <= n; i += lanes)
                    for (std::size_t k = 0; k < lanes; ++k) acc[k] = values[i + k] < acc[k] ? values[i + k] : acc[k];
                T best = acc[0];
                for (std::size_t k = 1; k < lanes; ++k) best = acc[k] < best ? acc[k] : best;
                for (; i < n; ++i) best = values[i] < best ? values[i] : best;
                return best;
            } else {
                T best = values[0];
                for (const T x : values) best = x < best ? x : best;
                return best;
            }
        }
        template <typename T>
        [[nodiscard]] inline T max_span(std::span<const T> values) {
            if constexpr (std::is_floating_point_v<T>) {
                constexpr std::size_t lanes = 16;
                T acc[lanes];
                for (std::size_t k = 0; k < lanes; ++k) acc[k] = values[0];
                std::size_t i = 0;
                const std::size_t n = values.size();
                for (; i + lanes <= n; i += lanes)
                    for (std::size_t k = 0; k < lanes; ++k) acc[k] = acc[k] < values[i + k] ? values[i + k] : acc[k];
                T best = acc[0];
                for (std::size_t k = 1; k < lanes; ++k) best = best < acc[k] ? acc[k] : best;
                for (; i < n; ++i) best = best < values[i] ? values[i] : best;
                return best;
            } else {
                T best = values[0];
                for (const T x : values) best = best < x ? x : best;
                return best;
            }
        }
    } // namespace Detail

    template <class R>
    concept ParIterable = std::ranges::viewable_range<R> && std::ranges::random_access_range<R> && std::ranges::sized_range<R>;

    namespace Detail {

        // =========================================================================================
        // Indexed sources: `size()` and a thread-safe, const `get(i)`.
        // =========================================================================================
        template <class S>
        concept IndexedSource = requires(const S &s, usize i) {
            { s.size() } -> std::convertible_to<usize>;
            s.get(i);
        };

        template <std::ranges::view V>
        struct RangeSource {
            mutable V view;
            /// Keeps storage alive when the data was moved in (rvalue containers, materialized inputs).
            std::shared_ptr<void> owner{};
            [[nodiscard]] usize size() const { return static_cast<usize>(std::ranges::size(view)); }
            [[nodiscard]] decltype(auto) get(usize i) const { return std::ranges::begin(view)[static_cast<std::ranges::range_difference_t<V>>(i)]; }
            /// Elements [begin, end) as one contiguous run -- only for contiguous ranges of numbers (enables SIMD reductions).
            [[nodiscard]] auto contiguous(usize begin, usize end) const
                requires(std::ranges::contiguous_range<V> && std::is_arithmetic_v<std::ranges::range_value_t<V>>)
            {
                using T = std::ranges::range_value_t<V>;
                return std::span<const T>(std::ranges::data(view) + begin, end - begin);
            }
        };

        template <IndexedSource S, class F>
        struct MapSource {
            S base;
            F fn;
            [[nodiscard]] usize size() const { return base.size(); }
            [[nodiscard]] decltype(auto) get(usize i) const { return std::invoke(fn, base.get(i)); }
        };

        template <IndexedSource S>
        struct EnumerateSource {
            S base;
            [[nodiscard]] usize size() const { return base.size(); }
            [[nodiscard]] auto get(usize i) const { return std::pair<usize, decltype(base.get(i))>(i, base.get(i)); }
        };

        template <IndexedSource A, IndexedSource B>
        struct ZipSource {
            A a;
            B b;
            [[nodiscard]] usize size() const { return (std::min)(a.size(), b.size()); }
            [[nodiscard]] auto get(usize i) const { return std::pair<decltype(a.get(i)), decltype(b.get(i))>(a.get(i), b.get(i)); }
        };

        template <IndexedSource S>
        struct RevSource {
            S base;
            [[nodiscard]] usize size() const { return base.size(); }
            [[nodiscard]] decltype(auto) get(usize i) const { return base.get(base.size() - 1 - i); }
        };

        template <IndexedSource S>
        struct SliceSource { // take / skip: [offset, offset + length)
            S base;
            usize offset;
            usize length;
            [[nodiscard]] usize size() const { return length; }
            [[nodiscard]] decltype(auto) get(usize i) const { return base.get(offset + i); }
            [[nodiscard]] auto contiguous(usize begin, usize end) const
                requires requires(const S &s) { s.contiguous(begin, end); }
            {
                return base.contiguous(offset + begin, offset + end);
            }
        };

        template <IndexedSource S>
        struct StepSource {
            S base;
            usize step;
            [[nodiscard]] usize size() const { return (base.size() + step - 1) / step; }
            [[nodiscard]] decltype(auto) get(usize i) const { return base.get(i * step); }
        };

        template <IndexedSource A, IndexedSource B>
        struct ChainSource {
            A a;
            B b;
            [[nodiscard]] usize size() const { return a.size() + b.size(); }
            [[nodiscard]] decltype(auto) get(usize i) const {
                using RA = decltype(a.get(i));
                using RB = decltype(b.get(i));
                static_assert(std::same_as<std::remove_cvref_t<RA>, std::remove_cvref_t<RB>>, "chain needs matching element types");
                if (i < a.size()) return static_cast<std::common_reference_t<RA, RB>>(a.get(i));
                return static_cast<std::common_reference_t<RA, RB>>(b.get(i - a.size()));
            }
        };

        template <IndexedSource S>
        struct ClonedSource {
            S base;
            [[nodiscard]] usize size() const { return base.size(); }
            [[nodiscard]] auto get(usize i) const { return std::remove_cvref_t<decltype(base.get(i))>(base.get(i)); }
        };

        template <class T>
        struct ChunksSource { // contiguous storage cut into spans of `chunk` (last may be shorter unless exact)
            T *data;
            usize length;
            usize chunk;
            [[nodiscard]] usize size() const { return (length + chunk - 1) / chunk; }
            [[nodiscard]] std::span<T> get(usize i) const { return {data + i * chunk, (std::min)(chunk, length - i * chunk)}; }
        };

        template <class T>
        struct WindowsSource { // every contiguous run of `window` elements
            T *data;
            usize length;
            usize window;
            [[nodiscard]] usize size() const { return length < window ? 0 : length - window + 1; }
            [[nodiscard]] std::span<T> get(usize i) const { return {data + i, window}; }
        };

        template <class T>
        struct SpanSource { // a contiguous run viewed as elements (used internally by chunk helpers)
            T *data;
            usize length;
            [[nodiscard]] usize size() const { return length; }
            [[nodiscard]] T &get(usize i) const { return data[i]; }
        };

    } // namespace Detail

    template <AsyncRuntime Rt, Detail::IndexedSource Src>
    class ParIter;
    template <AsyncRuntime Rt, class Base, class Stage, class Item>
    class ParFlow;

    namespace Detail {

        // =========================================================================================
        // Terminal operations, written once for ParIter and ParFlow.
        //
        // The derived class provides:
        //   usize par_size()                      -- number of base elements
        //   bool  leaf_try_fold(b, e, f)          -- feed elements of base range [b,e) through the pipeline into
        //                                            f(item) -> bool(continue); false once f asked to stop
        //   usize min_len(), max_len()            -- leaf size hints
        // =========================================================================================
        template <AsyncRuntime Rt, class D, class Item>
        class ParOps {
            [[nodiscard]] const D &self() const noexcept { return static_cast<const D &>(*this); }

            template <class Leaf>
            [[nodiscard]] auto leaves(StopFlag &stop, Leaf &&leaf) const {
                return run_leaves<Rt>(self().par_size(), self().min_len(), self().max_len(), stop, std::forward<Leaf>(leaf));
            }

          public:
            using item_type = Item;
            using value_type = std::remove_cvref_t<Item>;

            // ---- consumption ----
            template <class Fn>
                requires std::invocable<const Fn &, Item>
            void for_each(Fn fn) const {
                StopFlag stop;
                (void)leaves(stop, [&](usize b, usize e, usize) {
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        fn(std::forward<decltype(x)>(x));
                        return !stop.stopped();
                    });
                    return Unit{};
                });
            }

            /// `init()` runs once per leaf; the state is passed to every element of that leaf (scratch buffers, RNGs).
            template <class Init, class Fn>
            void for_each_init(Init init, Fn fn) const {
                StopFlag stop;
                (void)leaves(stop, [&](usize b, usize e, usize) {
                    auto state = init();
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        fn(state, std::forward<decltype(x)>(x));
                        return !stop.stopped();
                    });
                    return Unit{};
                });
            }

            /// `fn` returns false to stop everything early. Returns true if every element was visited.
            template <class Fn>
            [[nodiscard]] bool for_each_until(Fn fn) const {
                StopFlag stop;
                (void)leaves(stop, [&](usize b, usize e, usize) {
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (stop.stopped()) return false;
                        if (!fn(std::forward<decltype(x)>(x))) {
                            stop.stop();
                            return false;
                        }
                        return true;
                    });
                    return Unit{};
                });
                return !stop.stopped();
            }

            [[nodiscard]] usize count() const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    usize n = 0;
                    self().leaf_try_fold(b, e, [&](auto &&) {
                        ++n;
                        return true;
                    });
                    return n;
                });
                usize total = 0;
                for (auto &slot : slots) total += *slot;
                return total;
            }

            /// `op(acc, item)` combines an accumulator with an item and `op(acc, acc)` combines two accumulators,
            /// so `op` must be associative with `identity` as its identity element.
            template <class T, class Op>
            [[nodiscard]] T reduce(T identity, Op op) const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    T acc = identity;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        acc = op(std::move(acc), std::forward<decltype(x)>(x));
                        return true;
                    });
                    return acc;
                });
                T result = std::move(identity);
                for (auto &slot : slots) result = op(std::move(result), std::move(*slot));
                return result;
            }

            /// `fold_op(acc, item)` runs inside each leaf, `combine(acc, acc)` joins the leaves in order.
            template <class T, class FoldOp, class Combine>
            [[nodiscard]] T fold(T init, FoldOp fold_op, Combine combine) const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    T acc = init;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        acc = fold_op(std::move(acc), std::forward<decltype(x)>(x));
                        return true;
                    });
                    return acc;
                });
                T result = std::move(init);
                for (auto &slot : slots) result = combine(std::move(result), std::move(*slot));
                return result;
            }

            /// Same operation for both roles (works when the accumulator and item types coincide).
            template <class T, class Op>
            [[nodiscard]] T fold(T init, Op op) const {
                return fold(std::move(init), op, op);
            }

            [[nodiscard]] value_type sum() const {
                if constexpr (std::is_floating_point_v<value_type> && requires(const D &d) { d.contiguous_leaf(usize{}, usize{}); }) {
                    StopFlag stop;
                    auto slots = leaves(stop, [&](usize b, usize e, usize) { return sum_span<value_type>(self().contiguous_leaf(b, e)); });
                    value_type total{};
                    for (auto &slot : slots) total += *slot;
                    return total;
                } else {
                    return reduce(value_type{}, std::plus<>{});
                }
            }
            [[nodiscard]] value_type product() const { return reduce(value_type{1}, std::multiplies<>{}); }

            // ---- extremes: min returns the first minimum, max the last maximum (as Rust does) ----
            template <class Less>
            [[nodiscard]] std::optional<value_type> min_by(Less less) const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    std::optional<value_type> best;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (!best || less(x, *best)) best.emplace(std::forward<decltype(x)>(x));
                        return true;
                    });
                    return best;
                });
                std::optional<value_type> best;
                for (auto &slot : slots)
                    if (*slot && (!best || less(**slot, *best))) best = std::move(*slot);
                return best;
            }

            template <class Less>
            [[nodiscard]] std::optional<value_type> max_by(Less less) const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    std::optional<value_type> best;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (!best || !less(x, *best)) best.emplace(std::forward<decltype(x)>(x));
                        return true;
                    });
                    return best;
                });
                std::optional<value_type> best;
                for (auto &slot : slots)
                    if (*slot && (!best || !less(**slot, *best))) best = std::move(*slot);
                return best;
            }

            [[nodiscard]] std::optional<value_type> min() const {
                if constexpr (std::is_floating_point_v<value_type> && requires(const D &d) { d.contiguous_leaf(usize{}, usize{}); }) {
                    if (self().par_size() == 0) return std::nullopt;
                    StopFlag stop;
                    auto slots = leaves(stop, [&](usize b, usize e, usize) { return min_span<value_type>(self().contiguous_leaf(b, e)); });
                    value_type best = *slots.front();
                    for (auto &slot : slots) best = *slot < best ? *slot : best;
                    return best;
                } else {
                    return min_by(std::less<>{});
                }
            }
            [[nodiscard]] std::optional<value_type> max() const {
                if constexpr (std::is_floating_point_v<value_type> && requires(const D &d) { d.contiguous_leaf(usize{}, usize{}); }) {
                    if (self().par_size() == 0) return std::nullopt;
                    StopFlag stop;
                    auto slots = leaves(stop, [&](usize b, usize e, usize) { return max_span<value_type>(self().contiguous_leaf(b, e)); });
                    value_type best = *slots.front();
                    for (auto &slot : slots) best = best < *slot ? *slot : best;
                    return best;
                } else {
                    return max_by(std::less<>{});
                }
            }
            template <class Key>
            [[nodiscard]] std::optional<value_type> min_by_key(Key key) const {
                return min_by([&key](const auto &a, const auto &b) { return key(a) < key(b); });
            }
            template <class Key>
            [[nodiscard]] std::optional<value_type> max_by_key(Key key) const {
                return max_by([&key](const auto &a, const auto &b) { return key(a) < key(b); });
            }

            // ---- searching (short-circuiting) ----
            template <class Pred>
            [[nodiscard]] bool any(Pred pred) const {
                StopFlag stop;
                (void)leaves(stop, [&](usize b, usize e, usize) {
                    if (stop.stopped()) return Unit{};
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (stop.stopped()) return false;
                        if (pred(x)) {
                            stop.stop();
                            return false;
                        }
                        return true;
                    });
                    return Unit{};
                });
                return stop.stopped();
            }

            template <class Pred>
            [[nodiscard]] bool all(Pred pred) const {
                return !any([&pred](const auto &x) { return !pred(x); });
            }

            /// Some matching element (whichever leaf finds one first); stops the other leaves.
            template <class Pred>
            [[nodiscard]] std::optional<value_type> find_any(Pred pred) const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    std::optional<value_type> found;
                    if (stop.stopped()) return found;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (stop.stopped()) return false;
                        if (pred(x)) {
                            found.emplace(std::forward<decltype(x)>(x));
                            stop.stop();
                            return false;
                        }
                        return true;
                    });
                    return found;
                });
                for (auto &slot : slots)
                    if (*slot) return std::move(*slot);
                return std::nullopt;
            }

            /// The first matching element in iteration order; leaves after a hit are skipped or cut short.
            template <class Pred>
            [[nodiscard]] std::optional<value_type> find_first(Pred pred) const {
                std::atomic<usize> best_leaf{static_cast<usize>(-1)};
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize index) {
                    std::optional<value_type> found;
                    if (index > best_leaf.load(std::memory_order_relaxed)) return found;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        if (index > best_leaf.load(std::memory_order_relaxed)) return false;
                        if (pred(x)) {
                            found.emplace(std::forward<decltype(x)>(x));
                            usize current = best_leaf.load(std::memory_order_relaxed);
                            while (index < current && !best_leaf.compare_exchange_weak(current, index, std::memory_order_relaxed)) {
                            }
                            return false;
                        }
                        return true;
                    });
                    return found;
                });
                for (auto &slot : slots)
                    if (*slot) return std::move(*slot);
                return std::nullopt;
            }

            // ---- gathering ----
            /// Items in iteration order. `std::vector` by default; any container with an iterator-pair constructor
            /// via `to<C>()`.
            [[nodiscard]] std::vector<value_type> collect() const {
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    std::vector<value_type> local;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        local.emplace_back(std::forward<decltype(x)>(x));
                        return true;
                    });
                    return local;
                });
                usize total = 0;
                for (auto &slot : slots) total += slot->size();
                std::vector<value_type> out;
                out.reserve(total);
                for (auto &slot : slots) out.insert(out.end(), std::make_move_iterator(slot->begin()), std::make_move_iterator(slot->end()));
                return out;
            }

            template <class Container>
            [[nodiscard]] Container to() const {
                auto items = self().collect();
                return Container(std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
            }

            /// Appends the items (in order) to `out`.
            template <class Container>
            void collect_into(Container &out) const {
                auto items = self().collect();
                out.insert(out.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
            }

            /// Splits into (matching, not matching), each in iteration order.
            template <class Pred>
            [[nodiscard]] std::pair<std::vector<value_type>, std::vector<value_type>> partition(Pred pred) const {
                using Pair = std::pair<std::vector<value_type>, std::vector<value_type>>;
                StopFlag stop;
                auto slots = leaves(stop, [&](usize b, usize e, usize) {
                    Pair local;
                    self().leaf_try_fold(b, e, [&](auto &&x) {
                        (pred(x) ? local.first : local.second).emplace_back(std::forward<decltype(x)>(x));
                        return true;
                    });
                    return local;
                });
                Pair out;
                for (auto &slot : slots) {
                    out.first.insert(out.first.end(), std::make_move_iterator(slot->first.begin()), std::make_move_iterator(slot->first.end()));
                    out.second.insert(out.second.end(), std::make_move_iterator(slot->second.begin()), std::make_move_iterator(slot->second.end()));
                }
                return out;
            }

            /// For items that are pairs: two vectors, in order.
            [[nodiscard]] auto unzip() const {
                using A = std::remove_cvref_t<decltype(std::declval<value_type &>().first)>;
                using B = std::remove_cvref_t<decltype(std::declval<value_type &>().second)>;
                auto items = self().collect();
                std::pair<std::vector<A>, std::vector<B>> out;
                out.first.reserve(items.size());
                out.second.reserve(items.size());
                for (auto &item : items) {
                    out.first.emplace_back(std::move(item.first));
                    out.second.emplace_back(std::move(item.second));
                }
                return out;
            }
        };

    } // namespace Detail

    // =============================================================================================
    // ParFlow: a base iterator plus a per-element pipeline (`stage(item, emit) -> bool`; `emit` returns false to stop).
    // =============================================================================================
    template <AsyncRuntime Rt, class Base, class Stage, class Item>
    class ParFlow : public Detail::ParOps<Rt, ParFlow<Rt, Base, Stage, Item>, Item> {
      public:
        ParFlow(Base base, Stage stage) : base_(std::move(base)), stage_(std::move(stage)) {}

        [[nodiscard]] usize par_size() const { return base_.par_size(); }
        [[nodiscard]] usize min_len() const { return base_.min_len(); }
        [[nodiscard]] usize max_len() const { return base_.max_len(); }
        template <class F>
        bool leaf_try_fold(usize b, usize e, F &&f) const {
            return base_.leaf_try_fold(b, e, [&](auto &&x) { return stage_(std::forward<decltype(x)>(x), f); });
        }

        template <class Fn>
        [[nodiscard]] auto map(Fn fn) const {
            using Out = std::remove_cvref_t<std::invoke_result_t<const Fn &, Item>>;
            return chain<Out>([fn](auto &&x, auto &&emit) { return emit(fn(std::forward<decltype(x)>(x))); });
        }
        template <class Pred>
        [[nodiscard]] auto filter(Pred pred) const {
            return chain<Item>([pred](auto &&x, auto &&emit) {
                if (pred(x)) return emit(std::forward<decltype(x)>(x));
                return true;
            });
        }
        template <class Fn>
        [[nodiscard]] auto filter_map(Fn fn) const {
            using Opt = std::remove_cvref_t<std::invoke_result_t<const Fn &, Item>>;
            return chain<typename Opt::value_type>([fn](auto &&x, auto &&emit) {
                auto o = fn(std::forward<decltype(x)>(x));
                if (o) return emit(std::move(*o));
                return true;
            });
        }
        template <class Fn>
        [[nodiscard]] auto flat_map(Fn fn) const {
            using Inner = std::remove_cvref_t<std::invoke_result_t<const Fn &, Item>>;
            return chain<std::ranges::range_value_t<Inner>>([fn](auto &&x, auto &&emit) {
                for (auto &&y : fn(std::forward<decltype(x)>(x)))
                    if (!emit(std::forward<decltype(y)>(y))) return false;
                return true;
            });
        }
        /// Runs `fn` on each item as it passes (debugging, counters) without changing the stream.
        template <class Fn>
        [[nodiscard]] auto inspect(Fn fn) const {
            return chain<Item>([fn](auto &&x, auto &&emit) {
                fn(x);
                return emit(std::forward<decltype(x)>(x));
            });
        }

      private:
        template <class Out, class Next>
        [[nodiscard]] auto chain(Next next) const {
            auto composed = [first = stage_, next](auto &&x, auto &&emit) {
                return first(std::forward<decltype(x)>(x), [&](auto &&y) { return next(std::forward<decltype(y)>(y), emit); });
            };
            return ParFlow<Rt, Base, decltype(composed), Out>(base_, std::move(composed));
        }

        Base base_;
        Stage stage_;
    };

    // =============================================================================================
    // ParIter: random access.
    // =============================================================================================
    template <AsyncRuntime Rt, Detail::IndexedSource Src>
    class ParIter : public Detail::ParOps<Rt, ParIter<Rt, Src>, decltype(std::declval<const Src &>().get(0))> {
        using Item = decltype(std::declval<const Src &>().get(0));
        using Ops = Detail::ParOps<Rt, ParIter<Rt, Src>, Item>;

        template <class S>
        [[nodiscard]] ParIter<Rt, S> rebuild(S source) const {
            ParIter<Rt, S> out(std::move(source));
            out.min_len_ = min_len_;
            out.max_len_ = max_len_;
            return out;
        }
        template <AsyncRuntime R2, Detail::IndexedSource S2>
        friend class ParIter;

      public:
        explicit ParIter(Src source) : src_(std::move(source)) {}

        [[nodiscard]] const Src &source() const noexcept { return src_; }
        [[nodiscard]] usize par_size() const { return src_.size(); }
        [[nodiscard]] usize len() const { return src_.size(); }
        [[nodiscard]] bool is_empty() const { return src_.size() == 0; }
        [[nodiscard]] usize min_len() const noexcept { return min_len_; }
        [[nodiscard]] usize max_len() const noexcept { return max_len_; }
        /// Elements are known without running anything.
        [[nodiscard]] usize count() const { return src_.size(); }

        template <class F>
        bool leaf_try_fold(usize b, usize e, F &&f) const {
            for (usize i = b; i < e; ++i)
                if (!f(src_.get(i))) return false;
            return true;
        }

        /// For contiguous numeric sources: the leaf's elements as a span, so reductions can use vectorised kernels.
        [[nodiscard]] auto contiguous_leaf(usize b, usize e) const
            requires requires(const Src &s, usize i) { s.contiguous(i, i); }
        {
            return src_.contiguous(b, e);
        }

        /// Never split below `n` items per task (raise for cheap per-item work), or above `n` (lower for heavy ones).
        [[nodiscard]] ParIter with_min_len(usize n) const {
            ParIter copy = *this;
            copy.min_len_ = (std::max)(n, usize{1});
            return copy;
        }
        [[nodiscard]] ParIter with_max_len(usize n) const {
            ParIter copy = *this;
            copy.max_len_ = (std::max)(n, usize{1});
            return copy;
        }

        // ---- adapters that stay indexed ----
        template <class Fn>
            requires std::invocable<const Fn &, Item>
        [[nodiscard]] auto map(Fn fn) const {
            return rebuild(Detail::MapSource<Src, Fn>{src_, std::move(fn)});
        }
        /// Like `map`, with per-task state from `init()` passed first (see `for_each_init`). Eager: returns a vector.
        template <class Init, class Fn>
        [[nodiscard]] auto map_init(Init init, Fn fn) const {
            using T = std::remove_cvref_t<std::invoke_result_t<const Fn &, std::invoke_result_t<const Init &> &, Item>>;
            std::vector<T> out(src_.size());
            this->template run_indexed_init<T>(out, init, fn);
            return out;
        }
        [[nodiscard]] auto enumerate() const { return rebuild(Detail::EnumerateSource<Src>{src_}); }
        template <class Other>
        [[nodiscard]] auto zip(Other &&other) const {
            return rebuild(Detail::ZipSource<Src, decltype(source_of(std::forward<Other>(other)))>{src_, source_of(std::forward<Other>(other))});
        }
        template <class Other>
        [[nodiscard]] auto chain(Other &&other) const {
            return rebuild(Detail::ChainSource<Src, decltype(source_of(std::forward<Other>(other)))>{src_, source_of(std::forward<Other>(other))});
        }
        [[nodiscard]] auto rev() const { return rebuild(Detail::RevSource<Src>{src_}); }
        [[nodiscard]] auto take(usize n) const { return rebuild(Detail::SliceSource<Src>{src_, 0, (std::min)(n, src_.size())}); }
        [[nodiscard]] auto skip(usize n) const {
            const usize offset = (std::min)(n, src_.size());
            return rebuild(Detail::SliceSource<Src>{src_, offset, src_.size() - offset});
        }
        [[nodiscard]] auto step_by(usize n) const { return rebuild(Detail::StepSource<Src>{src_, (std::max)(n, usize{1})}); }
        /// Copies the referenced elements (like Rayon's `cloned`/`copied`).
        [[nodiscard]] auto cloned() const { return rebuild(Detail::ClonedSource<Src>{src_}); }
        [[nodiscard]] auto copied() const { return cloned(); }

        // ---- adapters that give up indices ----
        template <class Pred>
        [[nodiscard]] auto filter(Pred pred) const {
            return make_flow<Item>([pred](auto &&x, auto &&emit) {
                if (pred(x)) return emit(std::forward<decltype(x)>(x));
                return true;
            });
        }
        template <class Fn>
        [[nodiscard]] auto filter_map(Fn fn) const {
            using Opt = std::remove_cvref_t<std::invoke_result_t<const Fn &, Item>>;
            return make_flow<typename Opt::value_type>([fn](auto &&x, auto &&emit) {
                auto o = fn(std::forward<decltype(x)>(x));
                if (o) return emit(std::move(*o));
                return true;
            });
        }
        template <class Fn>
        [[nodiscard]] auto flat_map(Fn fn) const {
            using Inner = std::remove_cvref_t<std::invoke_result_t<const Fn &, Item>>;
            return make_flow<std::ranges::range_value_t<Inner>>([fn](auto &&x, auto &&emit) {
                for (auto &&y : fn(std::forward<decltype(x)>(x)))
                    if (!emit(std::forward<decltype(y)>(y))) return false;
                return true;
            });
        }
        template <class Fn>
        [[nodiscard]] auto inspect(Fn fn) const {
            return make_flow<Item>([fn](auto &&x, auto &&emit) {
                fn(x);
                return emit(std::forward<decltype(x)>(x));
            });
        }

        // ---- indexed terminals ----
        /// Items in order. Pre-sizes the result and lets every leaf write its own slice when the element type
        /// allows it, so there is no gather step.
        [[nodiscard]] auto collect() const {
            using T = typename Ops::value_type;
            if constexpr (std::is_default_constructible_v<T> && std::is_move_assignable_v<T> && !std::same_as<T, bool>) {
                std::vector<T> out(src_.size());
                Detail::StopFlag stop;
                (void)Detail::run_leaves<Rt>(src_.size(), min_len_, max_len_, stop, [&](usize b, usize e, usize) {
                    for (usize i = b; i < e; ++i) out[i] = src_.get(i);
                    return Unit{};
                });
                return out;
            } else {
                return Ops::collect();
            }
        }

        /// Source index of the first element satisfying `pred`.
        template <class Pred>
        [[nodiscard]] std::optional<usize> position_first(Pred pred) const {
            std::atomic<usize> best{static_cast<usize>(-1)};
            Detail::StopFlag stop;
            (void)Detail::run_leaves<Rt>(src_.size(), min_len_, max_len_, stop, [&](usize b, usize e, usize) {
                for (usize i = b; i < e && i < best.load(std::memory_order_relaxed); ++i) {
                    if (pred(src_.get(i))) {
                        usize current = best.load(std::memory_order_relaxed);
                        while (i < current && !best.compare_exchange_weak(current, i, std::memory_order_relaxed)) {
                        }
                        break;
                    }
                }
                return Unit{};
            });
            const usize found = best.load();
            return found == static_cast<usize>(-1) ? std::nullopt : std::optional<usize>(found);
        }

        /// Writes `fn(item)` for item i into `out[i]` (same length required).
        template <class Out, class Fn>
        void map_into(Out &out, Fn fn) const {
            Detail::StopFlag stop;
            (void)Detail::run_leaves<Rt>(src_.size(), min_len_, max_len_, stop, [&](usize b, usize e, usize) {
                for (usize i = b; i < e; ++i) out[i] = fn(src_.get(i));
                return Unit{};
            });
        }

      private:
        template <class T, class Out, class Init, class Fn>
        void run_indexed_init(Out &out, Init &init, Fn &fn) const {
            Detail::StopFlag stop;
            (void)Detail::run_leaves<Rt>(src_.size(), min_len_, max_len_, stop, [&](usize b, usize e, usize) {
                auto state = init();
                for (usize i = b; i < e; ++i) out[i] = fn(state, src_.get(i));
                return Unit{};
            });
        }

        template <class Out, class Stage>
        [[nodiscard]] auto make_flow(Stage stage) const {
            return ParFlow<Rt, ParIter, Stage, Out>(*this, std::move(stage));
        }

        // Accepts another ParIter or any random-access range as the second operand of zip/chain.
        template <class Other>
        [[nodiscard]] static auto source_of(Other &&other) {
            if constexpr (requires { other.source(); }) {
                return other.source();
            } else {
                auto v = std::views::all(std::forward<Other>(other));
                return Detail::RangeSource<decltype(v)>{std::move(v)};
            }
        }

        Src src_;
        usize min_len_ = 1;
        usize max_len_ = static_cast<usize>(-1);
    };

    namespace Detail {
        template <AsyncRuntime Rt>
        struct ParIterFn : std::ranges::range_adaptor_closure<ParIterFn<Rt>> {
            template <ParIterable R>
            [[nodiscard]] auto operator()(R &&range) const {
                using V = decltype(std::views::all(std::forward<R>(range)));
                if constexpr (std::copyable<V>) {
                    auto v = std::views::all(std::forward<R>(range));
                    return ParIter<Rt, RangeSource<decltype(v)>>(RangeSource<decltype(v)>{std::move(v)});
                } else { // an owning view: park the container on the heap so the iterator stays cheap to copy
                    auto holder = std::make_shared<std::remove_cvref_t<R>>(std::forward<R>(range));
                    auto v = std::views::all(*holder);
                    return ParIter<Rt, RangeSource<decltype(v)>>(RangeSource<decltype(v)>{v, holder});
                }
            }
            /// Single-pass inputs (directory iterators, generators) are materialized first, then processed in parallel.
            template <std::ranges::input_range R>
                requires(!ParIterable<R>)
            [[nodiscard]] auto operator()(R &&range) const {
                auto holder = std::make_shared<std::vector<std::ranges::range_value_t<R>>>(Foundation::to_par_iter(std::forward<R>(range)));
                auto v = std::views::all(*holder);
                return ParIter<Rt, RangeSource<decltype(v)>>(RangeSource<decltype(v)>{v, holder});
            }
        };

        template <AsyncRuntime Rt, class T>
        struct ChunkFn {
            template <std::ranges::contiguous_range R>
            [[nodiscard]] auto chunks(R &&range, usize n, bool exact) const {
                using Elem = std::remove_reference_t<std::ranges::range_reference_t<R>>;
                const usize length = static_cast<usize>(std::ranges::size(range));
                const usize k = (std::max)(n, usize{1});
                return ParIter<Rt, ChunksSource<Elem>>(ChunksSource<Elem>{std::ranges::data(range), exact ? length - length % k : length, k});
            }
        };
    } // namespace Detail

    inline constexpr Detail::ParIterFn<DefaultRuntime> par_iter{};
    template <AsyncRuntime Rt = DefaultRuntime>
    inline constexpr Detail::ParIterFn<Rt> par_iter_on{};

    /// Parallel iterator over `n` consecutive indices.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto par_range(usize n) {
        return par_iter_on<Rt>(std::views::iota(usize{0}, n));
    }

    /// `fn(i)` for every `i` in [0, n), in parallel.
    template <AsyncRuntime Rt = DefaultRuntime, class Fn>
    void par_for(usize n, Fn fn) {
        par_range<Rt>(n).for_each(std::move(fn));
    }

    /// Non-overlapping spans of `chunk_size` elements (the last may be shorter); spans are mutable if the range is.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::contiguous_range R>
    [[nodiscard]] auto par_chunks(R &&range, usize chunk_size) {
        return Detail::ChunkFn<Rt, void>{}.chunks(range, chunk_size, false);
    }
    /// Like `par_chunks` but drops a trailing partial chunk.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::contiguous_range R>
    [[nodiscard]] auto par_chunks_exact(R &&range, usize chunk_size) {
        return Detail::ChunkFn<Rt, void>{}.chunks(range, chunk_size, true);
    }
    /// Overlapping spans of `window` consecutive elements.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::contiguous_range R>
    [[nodiscard]] auto par_windows(R &&range, usize window) {
        using Elem = std::remove_reference_t<std::ranges::range_reference_t<R>>;
        return ParIter<Rt, Detail::WindowsSource<Elem>>(
            Detail::WindowsSource<Elem>{std::ranges::data(range), static_cast<usize>(std::ranges::size(range)), (std::max)(window, usize{1})});
    }

    /// Processes a single-pass range (directory listing, generator, parsed stream) in parallel *as it is produced*:
    /// the caller pulls items sequentially into batches that run on the pool, with a bounded number in flight, so
    /// memory stays flat for huge inputs. Order of processing is unspecified.
    template <AsyncRuntime Rt = DefaultRuntime, std::ranges::input_range R, class Fn>
    void par_bridge_for_each(R &&range, Fn fn, usize batch = 64) {
        using T = std::ranges::range_value_t<R>;
        const usize max_in_flight = (std::max)(usize{2}, current_num_threads<Rt>() * 4);
        std::deque<TaskHandle<void>> in_flight;
        std::exception_ptr error;
        auto reap = [&](bool all) {
            while (!in_flight.empty() && (all || in_flight.size() >= max_in_flight)) {
                try {
                    in_flight.front().wait();
                } catch (...) {
                    if (!error) error = std::current_exception();
                }
                in_flight.pop_front();
            }
        };
        std::vector<T> pending;
        auto flush = [&] {
            if (pending.empty()) return;
            reap(false);
            in_flight.push_back(Rt::spawn([items = std::move(pending), &fn]() mutable {
                for (auto &item : items) fn(item);
            }));
            pending = {};
        };
        for (auto &&item : range) {
            pending.emplace_back(std::forward<decltype(item)>(item));
            if (pending.size() >= batch) flush();
            if (error) break;
        }
        flush();
        reap(true);
        if (error) std::rethrow_exception(error);
    }

} // namespace SFT::Async
