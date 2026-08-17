#pragma once

#include <concepts>
#include <functional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

#include <Async/src/Chunk.hpp>
#include <Async/src/Ranges.hpp>
#include <Async/src/Runtime.hpp>
#include <Async/src/Task.hpp>

namespace SFT::Async {

    template <class R>
    concept ParIterable = std::ranges::viewable_range<R> && std::ranges::random_access_range<R> && std::ranges::sized_range<R>;

    template <AsyncRuntime Rt, class V>
        requires std::ranges::view<V> && ParIterable<V>
    class ParIter {
      public:
        /// Constructs a `ParIter` from the supplied initialization values.
        ///
        /// @param view `view` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit constexpr ParIter(V view) noexcept(std::is_nothrow_move_constructible_v<V>)
            : view_(std::move(view)) {}

        /// Maps the requested resource for access.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Fn>
            requires AsyncWork<Fn, std::ranges::range_reference_t<V>>
        [[nodiscard]] auto map(Fn fn) {
            auto result = std::views::transform(std::move(view_), std::move(fn));
            return ParIter<Rt, decltype(result)>(std::move(result));
        }

        /// Returns the count for this `ParIter`.
        ///
        /// @return Returns the current count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize count() const noexcept {
            return static_cast<usize>(std::ranges::size(view_));
        }

        /// Invokes the supplied function once for each element in the input range.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class Fn>
            requires AsyncWork<Fn, std::ranges::range_reference_t<V>>
        void for_each(Fn fn) {
            Ranges::for_each<Rt>(view_, std::move(fn));
        }

        /// Returns the current or globally available reduce value.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T, class Fn>
            requires AsyncWork<Fn, T, std::ranges::range_reference_t<V>> &&
                     std::convertible_to<std::invoke_result_t<const Fn &, T, std::ranges::range_reference_t<V>>, T>
        [[nodiscard]] T reduce(T identity, Fn combine) {
            const usize size = count();
            if (size == 0) {
                return identity;
            }

            const auto chunks = Detail::chunk_bounds(size, Detail::chunk_count_for<Rt>(size));
            auto first = std::ranges::begin(view_);
            using Diff = std::ranges::range_difference_t<V>;

            std::vector<TaskHandle<T>> handles;
            handles.reserve(chunks.size());
            for (const Detail::ChunkBounds &chunk : chunks) {
                auto chunk_begin = first + static_cast<Diff>(chunk.begin);
                auto chunk_end = first + static_cast<Diff>(chunk.end);
                handles.push_back(Rt::spawn([chunk_begin, chunk_end, identity, combine]() {
                    T acc = identity;
                    for (auto it = chunk_begin; it != chunk_end; ++it) {
                        acc = combine(std::move(acc), *it);
                    }
                    return acc;
                }));
            }

            T result = identity;
            for (TaskHandle<T> &handle : handles) {
                result = combine(std::move(result), handle.wait());
            }
            return result;
        }

        /// Returns the current or globally available sum value.
        ///
        /// @return Returns the current sum value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] auto sum() {
            using T = std::ranges::range_value_t<V>;
            return reduce(T{}, std::plus<>{});
        }

        /// Collects the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <template <class...> class Container = std::vector>
        [[nodiscard]] auto collect() {
            using T = std::ranges::range_value_t<V>;
            Container<T> out(count());
            Ranges::transform<Rt>(view_, out, [](auto &&value) -> T { return std::forward<decltype(value)>(value); });
            return out;
        }

      private:
        V view_;
    };

    namespace Detail {

        template <AsyncRuntime Rt>
        struct ParIterFn : std::ranges::range_adaptor_closure<ParIterFn<Rt>> {
            /// Invokes the callable behavior provided by `ParIterFn`.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            template <ParIterable R>
            [[nodiscard]] constexpr auto operator()(R &&range) const {
                auto view = std::views::all(std::forward<R>(range));
                return ParIter<Rt, decltype(view)>(std::move(view));
            }
        };

    } // namespace Detail

    inline constexpr Detail::ParIterFn<DefaultRuntime> par_iter{};

    template <AsyncRuntime Rt = DefaultRuntime>
    inline constexpr Detail::ParIterFn<Rt> par_iter_on{};

} // namespace SFT::Async
