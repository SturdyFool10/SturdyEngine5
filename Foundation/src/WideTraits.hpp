#pragma once


#include <Foundation/src/Wide.hpp>

#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <ostream>
#include <string>


using std::floor;
using std::hash;
using std::isinf;
using std::isnan;
using std::log10;
using std::ostream;
using std::string;
using std::to_string;

namespace SFT::Foundation {


    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(u128 v);
    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(i128 v);
    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(u256 v);
    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(i256 v);

    namespace Detail {

        /// Returns the current or globally available wide pow10 value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <class T>
        [[nodiscard]] constexpr T wide_pow10(int e) noexcept {
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


        /// Returns the current or globally available wide float to string value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <class T>
        [[nodiscard]] string wide_float_to_string(T v, int precision) {
            const f64 lead = static_cast<f64>(v);
            if (isnan(lead))
                return "nan";
            if (isinf(lead))
                return lead < 0 ? "-inf" : "inf";
            string out;
            if (v < T(0.0)) {
                out.push_back('-');
                v = -v;
            }
            if (!(v > T(0.0)))
                return "0";
            int e = static_cast<int>(floor(log10(static_cast<f64>(v))));
            T x = v / wide_pow10<T>(e);
            if (x >= T(10.0)) {
                x = x / T(10.0);
                ++e;
            }
            if (x < T(1.0)) {
                x = x * T(10.0);
                --e;
            }
            string digits;
            for (int i = 0; i <= precision; ++i) {
                int d = static_cast<int>(static_cast<f64>(x));
                d = d < 0 ? 0 : (d > 9 ? 9 : d);
                digits.push_back(static_cast<char>('0' + d));
                x = (x - T(static_cast<f64>(d))) * T(10.0);
            }
            out.push_back(digits[0]);
            out.push_back('.');
            out.append(digits, 1, string::npos);
            out.push_back('e');
            out.append(::to_string(e));
            return out;
        }
    } // namespace Detail

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(f128 v);
    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string to_string(const f256 &v);


    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const u256 &v);
    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const i256 &v);
    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, f128 v);
    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const f256 &v);

} // namespace SFT::Foundation


/// Writes or shifts the left-hand operand using the right-hand value.
///
/// @param os `os` value used by the operation.
/// @param v `v` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ostream &operator<<(ostream &os, __int128 v);
/// Writes or shifts the left-hand operand using the right-hand value.
///
/// @param os `os` value used by the operation.
/// @param v `v` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ostream &operator<<(ostream &os, unsigned __int128 v);


namespace std {

    template <>
    struct numeric_limits<SFT::Foundation::u256> {
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
        static constexpr float_denorm_style has_denorm = denorm_absent;
        static constexpr bool has_denorm_loss = false;
        static constexpr bool traps = false;
        static constexpr bool tinyness_before = false;
        static constexpr float_round_style round_style = round_toward_zero;
        /// Returns the current or globally available min value.
        ///
        /// @return Returns the current min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 min() noexcept { return {}; }
        /// Returns the current or globally available lowest value.
        ///
        /// @return Returns the current lowest value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 lowest() noexcept { return {}; }
        /// Returns the current or globally available max value.
        ///
        /// @return Returns the current max value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 max() noexcept {
            return SFT::Foundation::u256::from_parts(~static_cast<unsigned __int128>(0), ~static_cast<unsigned __int128>(0));
        }
        /// Returns the current or globally available epsilon value.
        ///
        /// @return Returns the current epsilon value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 epsilon() noexcept { return {}; }
        /// Rounds error using the supplied arguments and current state.
        ///
        /// @return Returns the current round error value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 round_error() noexcept { return {}; }
        /// Computes or queries `infinity` using the numeric semantics of `numeric_limits`.
        ///
        /// @return Returns the current infinity value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 infinity() noexcept { return {}; }
        /// Returns the current or globally available quiet na n value.
        ///
        /// @return Returns the current quiet na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 quiet_NaN() noexcept { return {}; }
        /// Signals the associated synchronization primitive or event.
        ///
        /// @return Returns the current signaling na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 signaling_NaN() noexcept { return {}; }
        /// Returns the current or globally available denorm min value.
        ///
        /// @return Returns the current denorm min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::u256 denorm_min() noexcept { return {}; }
    };

    template <>
    struct numeric_limits<SFT::Foundation::i256> {
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
        static constexpr float_denorm_style has_denorm = denorm_absent;
        static constexpr bool has_denorm_loss = false;
        static constexpr bool traps = false;
        static constexpr bool tinyness_before = false;
        static constexpr float_round_style round_style = round_toward_zero;
        /// Returns the current or globally available max value.
        ///
        /// @return Returns the current max value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 max() noexcept {
            return SFT::Foundation::i256::from_bits(
                SFT::Foundation::u256::from_parts(~static_cast<unsigned __int128>(0) >> 1, ~static_cast<unsigned __int128>(0)));
        }
        /// Returns the current or globally available min value.
        ///
        /// @return Returns the current min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 min() noexcept {
            return SFT::Foundation::i256::from_bits(
                SFT::Foundation::u256::from_parts(static_cast<unsigned __int128>(1) << 127, 0));
        }
        /// Returns the current or globally available lowest value.
        ///
        /// @return Returns the current lowest value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 lowest() noexcept { return min(); }
        /// Returns the current or globally available epsilon value.
        ///
        /// @return Returns the current epsilon value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 epsilon() noexcept { return {}; }
        /// Rounds error using the supplied arguments and current state.
        ///
        /// @return Returns the current round error value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 round_error() noexcept { return {}; }
        /// Computes or queries `infinity` using the numeric semantics of `numeric_limits`.
        ///
        /// @return Returns the current infinity value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 infinity() noexcept { return {}; }
        /// Returns the current or globally available quiet na n value.
        ///
        /// @return Returns the current quiet na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 quiet_NaN() noexcept { return {}; }
        /// Signals the associated synchronization primitive or event.
        ///
        /// @return Returns the current signaling na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 signaling_NaN() noexcept { return {}; }
        /// Returns the current or globally available denorm min value.
        ///
        /// @return Returns the current denorm min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::i256 denorm_min() noexcept { return {}; }
    };

    template <>
    struct numeric_limits<SFT::Foundation::f128> {
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
        static constexpr float_denorm_style has_denorm = denorm_present;
        static constexpr bool has_denorm_loss = false;
        static constexpr bool traps = false;
        static constexpr bool tinyness_before = false;
        static constexpr float_round_style round_style = round_to_nearest;
        static constexpr int radix = 2;
        static constexpr int digits = 106;
        static constexpr int digits10 = 31;
        static constexpr int max_digits10 = 33;
        static constexpr int min_exponent = -968;
        static constexpr int min_exponent10 = -291;
        static constexpr int max_exponent = 1024;
        static constexpr int max_exponent10 = 308;
        /// Returns the current or globally available min value.
        ///
        /// @return Returns the current min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 min() noexcept { return SFT::Foundation::f128(2.0041683600089728e-292); }
        /// Returns the current or globally available max value.
        ///
        /// @return Returns the current max value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 max() noexcept {
            return SFT::Foundation::f128(1.79769313486231570815e+308, 9.97920154767359795037e+291);
        }
        /// Returns the current or globally available lowest value.
        ///
        /// @return Returns the current lowest value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 lowest() noexcept { return -max(); }
        /// Returns the current or globally available epsilon value.
        ///
        /// @return Returns the current epsilon value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 epsilon() noexcept { return SFT::Foundation::f128(0x1p-104); }
        /// Rounds error using the supplied arguments and current state.
        ///
        /// @return Returns the current round error value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 round_error() noexcept { return SFT::Foundation::f128(0.5); }
        /// Computes or queries `infinity` using the numeric semantics of `numeric_limits`.
        ///
        /// @return Returns the current infinity value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 infinity() noexcept { return SFT::Foundation::f128(numeric_limits<SFT::f64>::infinity()); }
        /// Returns the current or globally available quiet na n value.
        ///
        /// @return Returns the current quiet na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 quiet_NaN() noexcept { return SFT::Foundation::f128(numeric_limits<SFT::f64>::quiet_NaN()); }
        /// Signals the associated synchronization primitive or event.
        ///
        /// @return Returns the current signaling na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 signaling_NaN() noexcept { return SFT::Foundation::f128(numeric_limits<SFT::f64>::signaling_NaN()); }
        /// Returns the current or globally available denorm min value.
        ///
        /// @return Returns the current denorm min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f128 denorm_min() noexcept { return SFT::Foundation::f128(numeric_limits<SFT::f64>::denorm_min()); }
    };

    template <>
    struct numeric_limits<SFT::Foundation::f256> {
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
        static constexpr float_denorm_style has_denorm = denorm_present;
        static constexpr bool has_denorm_loss = false;
        static constexpr bool traps = false;
        static constexpr bool tinyness_before = false;
        static constexpr float_round_style round_style = round_to_nearest;
        static constexpr int radix = 2;
        static constexpr int digits = 212;
        static constexpr int digits10 = 63;
        static constexpr int max_digits10 = 66;
        static constexpr int min_exponent = -862;
        static constexpr int min_exponent10 = -259;
        static constexpr int max_exponent = 1024;
        static constexpr int max_exponent10 = 308;
        /// Returns the current or globally available min value.
        ///
        /// @return Returns the current min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 min() noexcept { return SFT::Foundation::f256(1.6259745436952323e-260); }
        /// Returns the current or globally available max value.
        ///
        /// @return Returns the current max value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 max() noexcept {
            return SFT::Foundation::f256(1.79769313486231570815e+308, 9.97920154767359795037e+291, 5.53956966280111259858e+275, 3.07507889307840487279e+259);
        }
        /// Returns the current or globally available lowest value.
        ///
        /// @return Returns the current lowest value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 lowest() noexcept { return -max(); }
        /// Returns the current or globally available epsilon value.
        ///
        /// @return Returns the current epsilon value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 epsilon() noexcept { return SFT::Foundation::f256(0x1p-209); }
        /// Rounds error using the supplied arguments and current state.
        ///
        /// @return Returns the current round error value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 round_error() noexcept { return SFT::Foundation::f256(0.5); }
        /// Computes or queries `infinity` using the numeric semantics of `numeric_limits`.
        ///
        /// @return Returns the current infinity value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 infinity() noexcept { return SFT::Foundation::f256(numeric_limits<SFT::f64>::infinity()); }
        /// Returns the current or globally available quiet na n value.
        ///
        /// @return Returns the current quiet na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 quiet_NaN() noexcept { return SFT::Foundation::f256(numeric_limits<SFT::f64>::quiet_NaN()); }
        /// Signals the associated synchronization primitive or event.
        ///
        /// @return Returns the current signaling na n value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 signaling_NaN() noexcept { return SFT::Foundation::f256(numeric_limits<SFT::f64>::signaling_NaN()); }
        /// Returns the current or globally available denorm min value.
        ///
        /// @return Returns the current denorm min value.
        /// @note This function does not throw exceptions.
        static constexpr SFT::Foundation::f256 denorm_min() noexcept { return SFT::Foundation::f256(numeric_limits<SFT::f64>::denorm_min()); }
    };

} // namespace std


namespace SFT::Foundation::Detail {
    /// Hashes mix using the supplied arguments and current state.
    ///
    /// @param seed `seed` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize hash_mix(usize seed, usize value) noexcept;
    /// Hashes u128 using the supplied arguments and current state.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize hash_u128(u128 v) noexcept;
} // namespace SFT::Foundation::Detail

namespace std {

    template <>
    struct hash<SFT::Foundation::u256> {
        /// Invokes the callable behavior provided by `hash`.
        ///
        /// @param v `v` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::usize operator()(const SFT::Foundation::u256 &v) const noexcept;
    };
    template <>
    struct hash<SFT::Foundation::i256> {
        /// Invokes the callable behavior provided by `hash`.
        ///
        /// @param v `v` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::usize operator()(const SFT::Foundation::i256 &v) const noexcept;
    };
    template <>
    struct hash<SFT::Foundation::f128> {
        /// Invokes the callable behavior provided by `hash`.
        ///
        /// @param v `v` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::usize operator()(const SFT::Foundation::f128 &v) const noexcept;
    };
    template <>
    struct hash<SFT::Foundation::f256> {
        /// Invokes the callable behavior provided by `hash`.
        ///
        /// @param v `v` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] SFT::usize operator()(const SFT::Foundation::f256 &v) const noexcept;
    };


    template <>
    struct formatter<SFT::Foundation::u256> : formatter<string> {
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the formatting context iterator/result produced by the underlying formatter.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        auto format(const SFT::Foundation::u256 &v, auto &ctx) const {
            return formatter<string>::format(SFT::Foundation::to_string(v), ctx);
        }
    };
    template <>
    struct formatter<SFT::Foundation::i256> : formatter<string> {
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the formatting context iterator/result produced by the underlying formatter.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        auto format(const SFT::Foundation::i256 &v, auto &ctx) const {
            return formatter<string>::format(SFT::Foundation::to_string(v), ctx);
        }
    };
    template <>
    struct formatter<SFT::Foundation::f128> : formatter<string> {
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the formatting context iterator/result produced by the underlying formatter.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        auto format(SFT::Foundation::f128 v, auto &ctx) const {
            return formatter<string>::format(SFT::Foundation::to_string(v), ctx);
        }
    };
    template <>
    struct formatter<SFT::Foundation::f256> : formatter<string> {
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the formatting context iterator/result produced by the underlying formatter.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        auto format(const SFT::Foundation::f256 &v, auto &ctx) const {
            return formatter<string>::format(SFT::Foundation::to_string(v), ctx);
        }
    };

} // namespace std
