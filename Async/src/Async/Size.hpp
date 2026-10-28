#pragma once

#include <cstddef>
#include <limits>

namespace SFT::Async::Literals {

    namespace Detail {
        inline constexpr unsigned long long kibibyte = 1024ULL;
        inline constexpr unsigned long long mebibyte = kibibyte * kibibyte;
        inline constexpr unsigned long long gibibyte = mebibyte * kibibyte;
        inline constexpr unsigned long long tebibyte = gibibyte * kibibyte;
        inline constexpr unsigned long long pebibyte = tebibyte * kibibyte;
        inline constexpr unsigned long long kilobyte = 1000ULL;
        inline constexpr unsigned long long megabyte = kilobyte * kilobyte;
        inline constexpr unsigned long long gigabyte = megabyte * kilobyte;
        inline constexpr unsigned long long terabyte = gigabyte * kilobyte;
        inline constexpr unsigned long long petabyte = terabyte * kilobyte;

        template <unsigned long long Multiplier>
        consteval std::size_t checked_integer_size(unsigned long long value) {
            constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
            if (Multiplier > maximum || value > maximum / Multiplier) {
                throw "file size literal overflows std::size_t";
            }
            return static_cast<std::size_t>(value * Multiplier);
        }

        template <unsigned long long Multiplier>
        consteval std::size_t checked_floating_size(long double value) {
            constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
            const long double scaled = value * static_cast<long double>(Multiplier);
            if (!(value >= 0.0L) || !(scaled < static_cast<long double>(maximum))) {
                throw "file size literal is negative or overflows std::size_t";
            }
            return static_cast<std::size_t>(scaled);
        }
    } // namespace Detail

    /// Binary and decimal byte-size literals. Fractional sizes truncate down to the nearest byte.
    consteval std::size_t operator""_B(unsigned long long value) { return Detail::checked_integer_size<1>(value); }
    consteval std::size_t operator""_B(long double value) { return Detail::checked_floating_size<1>(value); }

    consteval std::size_t operator""_KiB(unsigned long long value) { return Detail::checked_integer_size<Detail::kibibyte>(value); }
    consteval std::size_t operator""_KiB(long double value) { return Detail::checked_floating_size<Detail::kibibyte>(value); }
    consteval std::size_t operator""_MiB(unsigned long long value) { return Detail::checked_integer_size<Detail::mebibyte>(value); }
    consteval std::size_t operator""_MiB(long double value) { return Detail::checked_floating_size<Detail::mebibyte>(value); }
    consteval std::size_t operator""_GiB(unsigned long long value) { return Detail::checked_integer_size<Detail::gibibyte>(value); }
    consteval std::size_t operator""_GiB(long double value) { return Detail::checked_floating_size<Detail::gibibyte>(value); }
    consteval std::size_t operator""_TiB(unsigned long long value) { return Detail::checked_integer_size<Detail::tebibyte>(value); }
    consteval std::size_t operator""_TiB(long double value) { return Detail::checked_floating_size<Detail::tebibyte>(value); }
    consteval std::size_t operator""_PiB(unsigned long long value) { return Detail::checked_integer_size<Detail::pebibyte>(value); }
    consteval std::size_t operator""_PiB(long double value) { return Detail::checked_floating_size<Detail::pebibyte>(value); }

    consteval std::size_t operator""_kB(unsigned long long value) { return Detail::checked_integer_size<Detail::kilobyte>(value); }
    consteval std::size_t operator""_kB(long double value) { return Detail::checked_floating_size<Detail::kilobyte>(value); }
    consteval std::size_t operator""_MB(unsigned long long value) { return Detail::checked_integer_size<Detail::megabyte>(value); }
    consteval std::size_t operator""_MB(long double value) { return Detail::checked_floating_size<Detail::megabyte>(value); }
    consteval std::size_t operator""_GB(unsigned long long value) { return Detail::checked_integer_size<Detail::gigabyte>(value); }
    consteval std::size_t operator""_GB(long double value) { return Detail::checked_floating_size<Detail::gigabyte>(value); }
    consteval std::size_t operator""_TB(unsigned long long value) { return Detail::checked_integer_size<Detail::terabyte>(value); }
    consteval std::size_t operator""_TB(long double value) { return Detail::checked_floating_size<Detail::terabyte>(value); }
    consteval std::size_t operator""_PB(unsigned long long value) { return Detail::checked_integer_size<Detail::petabyte>(value); }
    consteval std::size_t operator""_PB(long double value) { return Detail::checked_floating_size<Detail::petabyte>(value); }

} // namespace SFT::Async::Literals
