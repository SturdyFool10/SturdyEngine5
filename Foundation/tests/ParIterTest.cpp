#include <Foundation/ParIter.hpp>

#include <array>
#include <cassert>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

int main() {
    const std::array<int, 4> source{2, 3, 5, 7};
    const auto values = SFT::Foundation::to_par_iter(source);
    assert((values == std::vector<int>{2, 3, 5, 7}));
    static_assert(std::ranges::random_access_range<decltype(values)>);
    static_assert(std::ranges::sized_range<decltype(values)>);

    std::istringstream input{"one two three"};
    const std::istream_iterator<std::string> first{input};
    const std::istream_iterator<std::string> last{};
    const auto materialized = SFT::Foundation::to_par_iter(first, last);
    assert((materialized == std::vector<std::string>{"one", "two", "three"}));
    return 0;
}
