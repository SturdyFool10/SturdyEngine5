#pragma once

#include <concepts>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace SFT::Foundation {

    /// Materializes a single-pass input range into owned random-access storage suitable for
    /// `SFT::Async::par_iter`. The collection step is sequential; parallel work begins when
    /// the resulting vector is passed to Async's parallel iterator.
    template <std::input_iterator Iterator, std::sentinel_for<Iterator> Sentinel>
    [[nodiscard]] auto to_par_iter(Iterator first, Sentinel last) {
        using Value = std::iter_value_t<Iterator>;
        std::vector<Value> values;
        if constexpr (std::sized_sentinel_for<Sentinel, Iterator>) {
            const auto distance = last - first;
            if (distance > 0) values.reserve(static_cast<std::size_t>(distance));
        }
        for (; first != last; ++first) {
            values.emplace_back(*first);
        }
        return values;
    }

    /// Materializes an input range into owned random-access storage suitable for `Async::par_iter`.
    template <std::ranges::input_range Range>
    [[nodiscard]] auto to_par_iter(Range &&range) {
        return to_par_iter(std::ranges::begin(range), std::ranges::end(range));
    }

} // namespace SFT::Foundation
