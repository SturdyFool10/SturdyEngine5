#pragma once

#include "Foundation/Wide.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <ostream>
#include <string>

// Generic-library glue that lets the wide types stand in for built-in numbers everywhere the
// standard library reaches for traits: std::numeric_limits (generic numeric code), std::hash
// (unordered containers), std::formatter (std::format / logging), and stream insertion.
//
// Reach/limits note: std::is_arithmetic / std::is_integral / std::is_floating_point CANNOT be
// legally specialized for user types, so generic code gated on *those* will not treat u256/i256/
// f128/f256 as numbers. numeric_limits<>::is_specialized is the portable, standard-blessed probe,
// and it is wired up below. (i128/u128 are recognized by all of the above already — libstdc++
// treats __int128 as a true integral type.)

namespace SFT::Foundation {

    // --- Decimal conversion -----------------------------------------------------------------
    [[nodiscard]] inline std::string to_string(u128 v) {
        if (v == 0)
            return "0";
        char buf[40];
        int i = sizeof(buf);
        while (v != 0) {
            buf[--i] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        }
        return std::string(buf + i, buf + sizeof(buf));
    }
    [[nodiscard]] inline std::string to_string(i128 v) {
        const u128 mag = v < 0 ? (~static_cast<u128>(v) + 1) : static_cast<u128>(v); // handles INT128_MIN
        return v < 0 ? "-" + to_string(mag) : to_string(mag);
    }
    [[nodiscard]] inline std::string to_string(u256 v) {
        if (!static_cast<bool>(v))
            return "0";
        std::string s;
        while (static_cast<bool>(v)) {
            const auto [q, r] = u256::divmod(v, u256{10}); // fast 64-bit-divisor path
            s.push_back(static_cast<char>('0' + static_cast<int>(static_cast<u64>(r))));
            v = q;
        }
        return std::string(s.rbegin(), s.rend());
    }
    [[nodiscard]] inline std::string to_string(i256 v) {
        return v.is_negative() ? "-" + to_string((-v).bits) : to_string(v.bits);
    }

    namespace Detail {
        // 10^|e| in the wide-float type T (exact base, products rounded to T's precision).
        template <class T>
        [[nodiscard]] T wide_pow10(int e) noexcept {
            int n = e < 0 ? -e : e;
            T r(1.0), b(10.0);
            while (n != 0) {
                if (n & 1)
                    r = r * b;
                b = b * b;
                n >>= 1;
            }
            return e < 0 ? T(1.0) / r : r;
        }
        // Best-effort decimal (scientific) rendering of a wide float, good to `precision` digits.
        // Intended for logging/debugging — not guaranteed correctly-rounded or shortest round-trip.
        template <class T>
        [[nodiscard]] std::string wide_float_to_string(T v, int precision) {
            const f64 lead = static_cast<f64>(v);
            if (std::isnan(lead))
                return "nan";
            if (std::isinf(lead))
                return lead < 0 ? "-inf" : "inf";
            std::string out;
            if (v < T(0.0)) {
                out.push_back('-');
                v = -v;
            }
            if (!(v > T(0.0)))
                return "0";
            int e = static_cast<int>(std::floor(std::log10(static_cast<f64>(v))));
            T x = v / wide_pow10<T>(e);
            if (x >= T(10.0)) {
                x = x / T(10.0);
                ++e;
            }
            if (x < T(1.0)) {
                x = x * T(10.0);
                --e;
            }
            std::string digits;
            for (int i = 0; i <= precision; ++i) {
                int d = static_cast<int>(static_cast<f64>(x)); // x in [0,10)
                d = d < 0 ? 0 : (d > 9 ? 9 : d);
                digits.push_back(static_cast<char>('0' + d));
                x = (x - T(static_cast<f64>(d))) * T(10.0);
            }
            out.push_back(digits[0]);
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
            out.push_back('e');
            out.append(std::to_string(e));
            return out;
        }
    } // namespace Detail

    [[nodiscard]] inline std::string to_string(f128 v) { return Detail::wide_float_to_string(v, 31); }
    [[nodiscard]] inline std::string to_string(const f256 &v) { return Detail::wide_float_to_string(v, 62); }

    // --- Stream insertion (ADL-found for the program-defined wide types) --------------------
    inline std::ostream &operator<<(std::ostream &os, const u256 &v) { return os << to_string(v); }
    inline std::ostream &operator<<(std::ostream &os, const i256 &v) { return os << to_string(v); }
    inline std::ostream &operator<<(std::ostream &os, f128 v) { return os << to_string(v); }
    inline std::ostream &operator<<(std::ostream &os, const f256 &v) { return os << to_string(v); }

} // namespace SFT::Foundation

// std::ostream has no unambiguous overload for __int128; supply one so i128/u128 stream too.
inline std::ostream &operator<<(std::ostream &os, __int128 v) { return os << SFT::Foundation::to_string(v); }
inline std::ostream &operator<<(std::ostream &os, unsigned __int128 v) { return os << SFT::Foundation::to_string(v); }

// --- numeric_limits ------------------------------------------------------------------------
template <>
struct std::numeric_limits<SFT::Foundation::u256> {
    static constexpr bool is_specialized = true;
    static constexpr bool is_signed = false;
    static constexpr bool is_integer = true;
    static constexpr bool is_exact = true;
    static constexpr bool is_bounded = true;
    static constexpr bool is_modulo = true;
    static constexpr bool is_iec559 = false;
    static constexpr int radix = 2;
    static constexpr int digits = 256;
    static constexpr int digits10 = 77;
    static constexpr int max_digits10 = 0;
    static constexpr int min_exponent = 0;
    static constexpr int min_exponent10 = 0;
    static constexpr int max_exponent = 0;
    static constexpr int max_exponent10 = 0;
    static constexpr bool has_infinity = false;
    static constexpr bool has_quiet_NaN = false;
    static constexpr bool has_signaling_NaN = false;
    static constexpr std::float_denorm_style has_denorm = std::denorm_absent;
    static constexpr bool has_denorm_loss = false;
    static constexpr bool traps = false;
    static constexpr bool tinyness_before = false;
    static constexpr std::float_round_style round_style = std::round_toward_zero;
    static constexpr SFT::Foundation::u256 min() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 lowest() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 max() noexcept {
        return SFT::Foundation::u256::from_parts(~static_cast<unsigned __int128>(0), ~static_cast<unsigned __int128>(0));
    }
    static constexpr SFT::Foundation::u256 epsilon() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 round_error() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 infinity() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 quiet_NaN() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 signaling_NaN() noexcept { return {}; }
    static constexpr SFT::Foundation::u256 denorm_min() noexcept { return {}; }
};

template <>
struct std::numeric_limits<SFT::Foundation::i256> {
    static constexpr bool is_specialized = true;
    static constexpr bool is_signed = true;
    static constexpr bool is_integer = true;
    static constexpr bool is_exact = true;
    static constexpr bool is_bounded = true;
    static constexpr bool is_modulo = true;
    static constexpr bool is_iec559 = false;
    static constexpr int radix = 2;
    static constexpr int digits = 255;
    static constexpr int digits10 = 76;
    static constexpr int max_digits10 = 0;
    static constexpr int min_exponent = 0;
    static constexpr int min_exponent10 = 0;
    static constexpr int max_exponent = 0;
    static constexpr int max_exponent10 = 0;
    static constexpr bool has_infinity = false;
    static constexpr bool has_quiet_NaN = false;
    static constexpr bool has_signaling_NaN = false;
    static constexpr std::float_denorm_style has_denorm = std::denorm_absent;
    static constexpr bool has_denorm_loss = false;
    static constexpr bool traps = false;
    static constexpr bool tinyness_before = false;
    static constexpr std::float_round_style round_style = std::round_toward_zero;
    static constexpr SFT::Foundation::i256 max() noexcept {
        return SFT::Foundation::i256::from_bits(
            SFT::Foundation::u256::from_parts(~static_cast<unsigned __int128>(0) >> 1, ~static_cast<unsigned __int128>(0)));
    }
    static constexpr SFT::Foundation::i256 min() noexcept {
        return SFT::Foundation::i256::from_bits(
            SFT::Foundation::u256::from_parts(static_cast<unsigned __int128>(1) << 127, 0));
    }
    static constexpr SFT::Foundation::i256 lowest() noexcept { return min(); }
    static constexpr SFT::Foundation::i256 epsilon() noexcept { return {}; }
    static constexpr SFT::Foundation::i256 round_error() noexcept { return {}; }
    static constexpr SFT::Foundation::i256 infinity() noexcept { return {}; }
    static constexpr SFT::Foundation::i256 quiet_NaN() noexcept { return {}; }
    static constexpr SFT::Foundation::i256 signaling_NaN() noexcept { return {}; }
    static constexpr SFT::Foundation::i256 denorm_min() noexcept { return {}; }
};

template <>
struct std::numeric_limits<SFT::Foundation::f128> {
    static constexpr bool is_specialized = true;
    static constexpr bool is_signed = true;
    static constexpr bool is_integer = false;
    static constexpr bool is_exact = false;
    static constexpr bool is_bounded = true;
    static constexpr bool is_modulo = false;
    static constexpr bool is_iec559 = false; // composite of two f64s, not a single IEEE format
    static constexpr bool has_infinity = true;
    static constexpr bool has_quiet_NaN = true;
    static constexpr bool has_signaling_NaN = true;
    static constexpr std::float_denorm_style has_denorm = std::denorm_present;
    static constexpr bool has_denorm_loss = false;
    static constexpr bool traps = false;
    static constexpr bool tinyness_before = false;
    static constexpr std::float_round_style round_style = std::round_to_nearest;
    static constexpr int radix = 2;
    static constexpr int digits = 106; // 2 x 53
    static constexpr int digits10 = 31;
    static constexpr int max_digits10 = 33;
    static constexpr int min_exponent = -968;
    static constexpr int min_exponent10 = -291;
    static constexpr int max_exponent = 1024;
    static constexpr int max_exponent10 = 308;
    static constexpr SFT::Foundation::f128 min() noexcept { return SFT::Foundation::f128(2.0041683600089728e-292); }
    static constexpr SFT::Foundation::f128 max() noexcept {
        return SFT::Foundation::f128(1.79769313486231570815e+308, 9.97920154767359795037e+291);
    }
    static constexpr SFT::Foundation::f128 lowest() noexcept { return -max(); }
    static constexpr SFT::Foundation::f128 epsilon() noexcept { return SFT::Foundation::f128(0x1p-104); }
    static constexpr SFT::Foundation::f128 round_error() noexcept { return SFT::Foundation::f128(0.5); }
    static SFT::Foundation::f128 infinity() noexcept { return SFT::Foundation::f128(std::numeric_limits<double>::infinity()); }
    static SFT::Foundation::f128 quiet_NaN() noexcept { return SFT::Foundation::f128(std::numeric_limits<double>::quiet_NaN()); }
    static SFT::Foundation::f128 signaling_NaN() noexcept { return SFT::Foundation::f128(std::numeric_limits<double>::signaling_NaN()); }
    static SFT::Foundation::f128 denorm_min() noexcept { return SFT::Foundation::f128(std::numeric_limits<double>::denorm_min()); }
};

template <>
struct std::numeric_limits<SFT::Foundation::f256> {
    static constexpr bool is_specialized = true;
    static constexpr bool is_signed = true;
    static constexpr bool is_integer = false;
    static constexpr bool is_exact = false;
    static constexpr bool is_bounded = true;
    static constexpr bool is_modulo = false;
    static constexpr bool is_iec559 = false;
    static constexpr bool has_infinity = true;
    static constexpr bool has_quiet_NaN = true;
    static constexpr bool has_signaling_NaN = true;
    static constexpr std::float_denorm_style has_denorm = std::denorm_present;
    static constexpr bool has_denorm_loss = false;
    static constexpr bool traps = false;
    static constexpr bool tinyness_before = false;
    static constexpr std::float_round_style round_style = std::round_to_nearest;
    static constexpr int radix = 2;
    static constexpr int digits = 212; // 4 x 53
    static constexpr int digits10 = 63;
    static constexpr int max_digits10 = 66;
    static constexpr int min_exponent = -862;
    static constexpr int min_exponent10 = -259;
    static constexpr int max_exponent = 1024;
    static constexpr int max_exponent10 = 308;
    static constexpr SFT::Foundation::f256 min() noexcept { return SFT::Foundation::f256(1.6259745436952323e-260); }
    static constexpr SFT::Foundation::f256 max() noexcept {
        return SFT::Foundation::f256(1.79769313486231570815e+308, 9.97920154767359795037e+291, 5.53956966280111259858e+275, 3.07507889307840487279e+259);
    }
    static constexpr SFT::Foundation::f256 lowest() noexcept { return -max(); }
    static constexpr SFT::Foundation::f256 epsilon() noexcept { return SFT::Foundation::f256(0x1p-209); }
    static constexpr SFT::Foundation::f256 round_error() noexcept { return SFT::Foundation::f256(0.5); }
    static SFT::Foundation::f256 infinity() noexcept { return SFT::Foundation::f256(std::numeric_limits<double>::infinity()); }
    static SFT::Foundation::f256 quiet_NaN() noexcept { return SFT::Foundation::f256(std::numeric_limits<double>::quiet_NaN()); }
    static SFT::Foundation::f256 signaling_NaN() noexcept { return SFT::Foundation::f256(std::numeric_limits<double>::signaling_NaN()); }
    static SFT::Foundation::f256 denorm_min() noexcept { return SFT::Foundation::f256(std::numeric_limits<double>::denorm_min()); }
};

// --- std::hash -----------------------------------------------------------------------------
namespace SFT::Foundation::Detail {
    [[nodiscard]] inline std::size_t hash_mix(std::size_t seed, std::size_t value) noexcept {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }
    [[nodiscard]] inline std::size_t hash_u128(u128 v) noexcept {
        const std::hash<u64> h;
        return hash_mix(h(static_cast<u64>(v)), h(static_cast<u64>(v >> 64)));
    }
} // namespace SFT::Foundation::Detail

template <>
struct std::hash<SFT::Foundation::u256> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::u256 &v) const noexcept {
        return SFT::Foundation::Detail::hash_mix(SFT::Foundation::Detail::hash_u128(v.lo),
                                                 SFT::Foundation::Detail::hash_u128(v.hi));
    }
};
template <>
struct std::hash<SFT::Foundation::i256> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::i256 &v) const noexcept {
        return std::hash<SFT::Foundation::u256>{}(v.bits);
    }
};
template <>
struct std::hash<SFT::Foundation::f128> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::f128 &v) const noexcept {
        const std::hash<SFT::f64> h;
        // normalize -0.0 so +0.0 and -0.0 (which compare equal) hash equally
        return SFT::Foundation::Detail::hash_mix(h(v.hi == 0.0 ? 0.0 : v.hi), h(v.lo == 0.0 ? 0.0 : v.lo));
    }
};
template <>
struct std::hash<SFT::Foundation::f256> {
    [[nodiscard]] std::size_t operator()(const SFT::Foundation::f256 &v) const noexcept {
        const std::hash<SFT::f64> h;
        std::size_t seed = h(v.x[0] == 0.0 ? 0.0 : v.x[0]);
        for (int i = 1; i < 4; ++i)
            seed = SFT::Foundation::Detail::hash_mix(seed, h(v.x[i] == 0.0 ? 0.0 : v.x[i]));
        return seed;
    }
};

// --- std::formatter (inherits string formatter -> supports fill/align/width) ---------------
template <>
struct std::formatter<SFT::Foundation::u256> : std::formatter<std::string> {
    auto format(const SFT::Foundation::u256 &v, auto &ctx) const {
        return std::formatter<std::string>::format(SFT::Foundation::to_string(v), ctx);
    }
};
template <>
struct std::formatter<SFT::Foundation::i256> : std::formatter<std::string> {
    auto format(const SFT::Foundation::i256 &v, auto &ctx) const {
        return std::formatter<std::string>::format(SFT::Foundation::to_string(v), ctx);
    }
};
template <>
struct std::formatter<SFT::Foundation::f128> : std::formatter<std::string> {
    auto format(SFT::Foundation::f128 v, auto &ctx) const {
        return std::formatter<std::string>::format(SFT::Foundation::to_string(v), ctx);
    }
};
template <>
struct std::formatter<SFT::Foundation::f256> : std::formatter<std::string> {
    auto format(const SFT::Foundation::f256 &v, auto &ctx) const {
        return std::formatter<std::string>::format(SFT::Foundation::to_string(v), ctx);
    }
};
