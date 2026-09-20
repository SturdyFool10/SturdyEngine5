#pragma once

#include <Foundation/Foundation.hpp>

#include <string_view>

namespace SFT::Reflection::Detail {


    /// A compile-time string usable as a non-type template parameter (a "structural type" per
    /// C++20's NTTP rules) — lets call sites name a field/method/event by string literal as a
    /// template argument, e.g. `field<"health">()` (`StaticReflection.hpp`) or
    /// `SFT_REFLECT_FIRE_EVENT`'s per-`(T, event name)` caching (`Invoke.hpp`).
    template <usize N>
    struct FixedString {
        char data[N]{};

        consteval FixedString(const char (&str)[N]) noexcept {
            for (usize i = 0; i < N; ++i) {
                data[i] = str[i];
            }
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return std::string_view(data, N - 1);
        }
    };


} // namespace SFT::Reflection::Detail
