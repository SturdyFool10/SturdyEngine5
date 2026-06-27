module;

#include <bit>
#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>

export module Sturdy.Foundation:Math;

import :Types;
import :Wide;
import :Constants;
import :NumericConcepts;

using std::acos;
using std::asin;
using std::atan;
using std::atan2;
using std::bit_cast;
using std::cbrt;
using std::ceil;
using std::copysign;
using std::cos;
using std::cosh;
using std::exp;
using std::fabs;
using std::floating_point;
using std::floor;
using std::fma;
using std::fmod;
using std::hypot;
using std::integral;
using std::is_constant_evaluated;
using std::is_unsigned_v;
using std::isfinite;
using std::isinf;
using std::isnan;
using std::ldexp;
using std::lerp;
using std::log;
using std::log10;
using std::log2;
using std::numeric_limits;
using std::pow;
using std::remainder;
using std::remove_cvref_t;
using std::round;
using std::same_as;
using std::signbit;
using std::sin;
using std::sinh;
using std::sqrt;
using std::tan;
using std::tanh;
using std::trunc;

export namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        concept WideNumber = same_as<remove_cvref_t<T>, u256> || same_as<remove_cvref_t<T>, i256> || WideFloat<T>;

        template <WideFloat T>
        inline constexpr int series_iterations = same_as<remove_cvref_t<T>, f128> ? 64 : 128;

        template <WideFloat T>
        inline constexpr int refinement_iterations = same_as<remove_cvref_t<T>, f128> ? 3 : 5;

        inline constexpr u64 f64_sign_mask = 0x8000000000000000ULL;
        inline constexpr u64 f64_exponent_mask = 0x7ff0000000000000ULL;
        inline constexpr u64 f64_fraction_mask = 0x000fffffffffffffULL;
        inline constexpr u64 f64_magnitude_mask = 0x7fffffffffffffffULL;
        inline constexpr u64 f64_exponent_bias_bits = 0x3ff0000000000000ULL;

        [[nodiscard]] constexpr u64 f64_bits(f64 x) noexcept { return bit_cast<u64>(x); }
        [[nodiscard]] constexpr f64 f64_from_bits(u64 bits) noexcept { return bit_cast<f64>(bits); }
        [[nodiscard]] constexpr f64 quiet_nan_f64() noexcept { return f64_from_bits(0x7ff8000000000000ULL); }
        [[nodiscard]] constexpr f64 infinity_f64() noexcept { return f64_from_bits(f64_exponent_mask); }
        [[nodiscard]] constexpr f64 abs_f64(f64 x) noexcept { return f64_from_bits(f64_bits(x) & f64_magnitude_mask); }
        [[nodiscard]] constexpr bool f64_isnan(f64 x) noexcept { return (f64_bits(x) & f64_magnitude_mask) > f64_exponent_mask; }
        [[nodiscard]] constexpr bool f64_isinf(f64 x) noexcept { return (f64_bits(x) & f64_magnitude_mask) == f64_exponent_mask; }
        [[nodiscard]] constexpr bool f64_isfinite(f64 x) noexcept { return (f64_bits(x) & f64_exponent_mask) != f64_exponent_mask; }
        [[nodiscard]] constexpr bool f64_signbit(f64 x) noexcept { return (f64_bits(x) & f64_sign_mask) != 0; }

        struct F64Parts {
            f64 significand;
            int exponent;
        };

        [[nodiscard]] constexpr F64Parts decompose_positive_f64(f64 x) noexcept {
            u64 bits = f64_bits(x);
            int raw_exponent = static_cast<int>((bits & f64_exponent_mask) >> 52);
            int exponent_adjust = 0;
            if (raw_exponent == 0) {
                x *= 0x1p54;
                bits = f64_bits(x);
                raw_exponent = static_cast<int>((bits & f64_exponent_mask) >> 52);
                exponent_adjust = -54;
            }
            return {f64_from_bits((bits & f64_fraction_mask) | f64_exponent_bias_bits), raw_exponent - 1023 + exponent_adjust};
        }

        [[nodiscard]] constexpr f64 ldexp_f64(f64 x, int exp) noexcept {
            if (!is_constant_evaluated())
                return ::ldexp(x, exp);
            if (x == 0.0 || !f64_isfinite(x) || exp == 0)
                return x;

            const bool negative = f64_signbit(x);
            if (exp > 2048)
                return negative ? -infinity_f64() : infinity_f64();
            if (exp < -2048)
                return negative ? f64_from_bits(f64_sign_mask) : 0.0;

            const F64Parts parts = decompose_positive_f64(abs_f64(x));
            const int result_exponent = parts.exponent + exp;
            if (result_exponent > 1023)
                return negative ? -infinity_f64() : infinity_f64();
            if (result_exponent < -1074)
                return negative ? f64_from_bits(f64_sign_mask) : 0.0;

            while (exp >= 512) {
                x *= 0x1p512;
                exp -= 512;
            }
            while (exp <= -512) {
                x *= 0x1p-512;
                exp += 512;
            }
            while (exp > 0) {
                x *= 2.0;
                --exp;
            }
            while (exp < 0) {
                x *= 0.5;
                ++exp;
            }
            return x;
        }

        [[nodiscard]] constexpr f64 trunc_f64(f64 x) noexcept {
            if (!f64_isfinite(x) || x == 0.0 || abs_f64(x) >= 0x1p52)
                return x;
            const i64 integral = static_cast<i64>(x);
            if (integral == 0 && f64_signbit(x))
                return f64_from_bits(f64_sign_mask);
            return static_cast<f64>(integral);
        }

        [[nodiscard]] constexpr f64 floor_f64(f64 x) noexcept {
            if (!f64_isfinite(x) || x == 0.0 || abs_f64(x) >= 0x1p52)
                return x;
            const f64 truncated = trunc_f64(x);
            return truncated > x ? truncated - 1.0 : truncated;
        }

        [[nodiscard]] constexpr f64 ceil_f64(f64 x) noexcept {
            if (!f64_isfinite(x) || x == 0.0 || abs_f64(x) >= 0x1p52)
                return x;
            const f64 truncated = trunc_f64(x);
            return truncated < x ? truncated + 1.0 : truncated;
        }

        [[nodiscard]] constexpr f64 round_f64(f64 x) noexcept {
            if (!f64_isfinite(x) || x == 0.0 || abs_f64(x) >= 0x1p52)
                return x;
            return x < 0.0 ? ceil_f64(x - 0.5) : floor_f64(x + 0.5);
        }

        [[nodiscard]] constexpr f64 sqrt_f64(f64 x) noexcept {
            if (!is_constant_evaluated())
                return ::sqrt(x);
            if (f64_isnan(x) || x == 0.0)
                return x;
            if (x < 0.0)
                return quiet_nan_f64();
            if (f64_isinf(x))
                return x;

            f64 y = f64_from_bits((f64_bits(x) >> 1) + 0x1ff8000000000000ULL);
            for (int i = 0; i < 8; ++i)
                y = 0.5 * (y + x / y);
            return y;
        }

        [[nodiscard]] constexpr f64 cbrt_f64(f64 x) noexcept {
            if (!is_constant_evaluated())
                return ::cbrt(x);
            if (f64_isnan(x) || x == 0.0 || f64_isinf(x))
                return x;

            const bool negative = x < 0.0;
            const F64Parts parts = decompose_positive_f64(negative ? -x : x);
            int quotient = parts.exponent / 3; // floor-divide the exponent so the remainder stays in [0,3)
            if (parts.exponent % 3 < 0)
                --quotient;
            const int remainder = parts.exponent - quotient * 3;
            const f64 reduced = ldexp_f64(parts.significand, remainder);

            f64 y = 1.0 + (reduced - 1.0) / 3.0;
            for (int i = 0; i < 8; ++i)
                y = (2.0 * y + reduced / (y * y)) / 3.0;
            y = ldexp_f64(y, quotient);
            return negative ? -y : y;
        }

        [[nodiscard]] constexpr f64 log_f64(f64 x) noexcept {
            if (!is_constant_evaluated())
                return ::log(x);
            if (f64_isnan(x) || x < 0.0)
                return quiet_nan_f64();
            if (x == 0.0)
                return -infinity_f64();
            if (f64_isinf(x))
                return infinity_f64();

            F64Parts parts = decompose_positive_f64(x);
            if (parts.significand > 0x1.6a09e667f3bcdp+0) { // sqrt(2)
                parts.significand *= 0.5;
                ++parts.exponent;
            }

            const f64 y = (parts.significand - 1.0) / (parts.significand + 1.0);
            const f64 y2 = y * y;
            f64 term = y;
            f64 sum = 0.0;
            for (int denominator = 1; denominator <= 99; denominator += 2) {
                sum += term / static_cast<f64>(denominator);
                term *= y2;
            }
            return 2.0 * sum + static_cast<f64>(parts.exponent) * 0x1.62e42fefa39efp-1;
        }

        [[nodiscard]] constexpr f64 atan_series_f64(f64 x) noexcept {
            const f64 x2 = x * x;
            f64 term = x;
            f64 sum = x;
            for (int denominator = 3, sign = -1; denominator <= 159; denominator += 2, sign = -sign) {
                term *= x2;
                const f64 contribution = term / static_cast<f64>(denominator);
                sum = sign < 0 ? sum - contribution : sum + contribution;
            }
            return sum;
        }

        [[nodiscard]] constexpr f64 atan_f64(f64 x) noexcept {
            if (!is_constant_evaluated())
                return ::atan(x);
            if (f64_isnan(x))
                return x;
            if (f64_isinf(x))
                return f64_signbit(x) ? -0x1.921fb54442d18p+0 : 0x1.921fb54442d18p+0;

            const bool negative = x < 0.0;
            x = negative ? -x : x;

            f64 result = 0.0;
            if (x > 1.0) {
                result = 0x1.921fb54442d18p+0 - atan_f64(1.0 / x);
            } else if (x > 0x1.a827999fcef32p-2) { // sqrt(2) - 1
                result = 0x1.921fb54442d18p-1 + atan_series_f64((x - 1.0) / (x + 1.0));
            } else {
                result = atan_series_f64(x);
            }
            return negative ? -result : result;
        }

        [[nodiscard]] constexpr f64 asin_f64(f64 x) noexcept {
            if (!is_constant_evaluated())
                return ::asin(x);
            if (f64_isnan(x))
                return x;
            if (x < -1.0 || x > 1.0)
                return quiet_nan_f64();
            if (x == 1.0)
                return 0x1.921fb54442d18p+0;
            if (x == -1.0)
                return -0x1.921fb54442d18p+0;
            return atan_f64(x / sqrt_f64((1.0 - x) * (1.0 + x)));
        }

        [[nodiscard]] constexpr f64 remainder_f64(f64 x, f64 y) noexcept {
            if (!is_constant_evaluated())
                return ::remainder(x, y);
            if (y == 0.0 || f64_isnan(x) || f64_isnan(y) || f64_isinf(x))
                return quiet_nan_f64();
            if (f64_isinf(y))
                return x;
            return x - round_f64(x / y) * y;
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T quiet_nan() noexcept {
            return T(quiet_nan_f64());
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T infinity() noexcept {
            return T(infinity_f64());
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T negative_infinity() noexcept {
            return -infinity<T>();
        }

    } // namespace Detail

    // --- classification -----------------------------------------------------------------------
    template <integral T>
    [[nodiscard]] constexpr bool isnan(T) noexcept {
        return false;
    }
    template <floating_point T>
    [[nodiscard]] inline bool isnan(T x) noexcept {
        return ::isnan(x);
    }
    [[nodiscard]] constexpr bool isnan(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool isnan(i256) noexcept { return false; }
    [[nodiscard]] constexpr bool isnan(f128 x) noexcept { return Detail::f64_isnan(x.hi) || Detail::f64_isnan(x.lo); }
    [[nodiscard]] constexpr bool isnan(const f256 &x) noexcept {
        return Detail::f64_isnan(x.x[0]) || Detail::f64_isnan(x.x[1]) || Detail::f64_isnan(x.x[2]) || Detail::f64_isnan(x.x[3]);
    }

    template <integral T>
    [[nodiscard]] constexpr bool isinf(T) noexcept {
        return false;
    }
    template <floating_point T>
    [[nodiscard]] inline bool isinf(T x) noexcept {
        return ::isinf(x);
    }
    [[nodiscard]] constexpr bool isinf(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool isinf(i256) noexcept { return false; }
    [[nodiscard]] constexpr bool isinf(f128 x) noexcept { return Detail::f64_isinf(x.hi); }
    [[nodiscard]] constexpr bool isinf(const f256 &x) noexcept { return Detail::f64_isinf(x.x[0]); }

    template <integral T>
    [[nodiscard]] constexpr bool isfinite(T) noexcept {
        return true;
    }
    template <floating_point T>
    [[nodiscard]] inline bool isfinite(T x) noexcept {
        return ::isfinite(x);
    }
    [[nodiscard]] constexpr bool isfinite(u256) noexcept { return true; }
    [[nodiscard]] constexpr bool isfinite(i256) noexcept { return true; }
    [[nodiscard]] constexpr bool isfinite(f128 x) noexcept { return Detail::f64_isfinite(x.hi) && Detail::f64_isfinite(x.lo); }
    [[nodiscard]] constexpr bool isfinite(const f256 &x) noexcept {
        return Detail::f64_isfinite(x.x[0]) && Detail::f64_isfinite(x.x[1]) && Detail::f64_isfinite(x.x[2]) && Detail::f64_isfinite(x.x[3]);
    }

    template <integral T>
    [[nodiscard]] constexpr bool signbit(T x) noexcept {
        if constexpr (is_unsigned_v<T>)
            return false;
        else
            return x < 0;
    }
    template <floating_point T>
    [[nodiscard]] inline bool signbit(T x) noexcept {
        return ::signbit(x);
    }
    [[nodiscard]] constexpr bool signbit(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool signbit(i256 x) noexcept { return x.is_negative(); }
    [[nodiscard]] constexpr bool signbit(f128 x) noexcept { return Detail::f64_signbit(x.hi) || (x.hi == 0.0 && Detail::f64_signbit(x.lo)); }
    [[nodiscard]] constexpr bool signbit(const f256 &x) noexcept {
        return Detail::f64_signbit(x.x[0]) ||
               (x.x[0] == 0.0 && (Detail::f64_signbit(x.x[1]) || Detail::f64_signbit(x.x[2]) || Detail::f64_signbit(x.x[3])));
    }

    template <class T>
    [[nodiscard]] constexpr bool is_nan(T x) noexcept {
        return isnan(x);
    }
    template <class T>
    [[nodiscard]] constexpr bool is_inf(T x) noexcept {
        return isinf(x);
    }
    template <class T>
    [[nodiscard]] constexpr bool is_finite(T x) noexcept {
        return isfinite(x);
    }

    // --- abs / sign / copy sign ---------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T abs(T x) noexcept {
        return ::fabs(x);
    }
    template <integral T>
    [[nodiscard]] constexpr T abs(T x) noexcept {
        if constexpr (is_unsigned_v<T>) {
            return x;
        } else {
            return x < 0 ? -x : x;
        }
    }
    [[nodiscard]] constexpr u128 abs(u128 x) noexcept { return x; }
    [[nodiscard]] constexpr i128 abs(i128 x) noexcept { return x < 0 ? -x : x; }
    [[nodiscard]] constexpr u256 abs(u256 x) noexcept { return x; }
    [[nodiscard]] constexpr i256 abs(i256 x) noexcept { return x.is_negative() ? -x : x; }
    [[nodiscard]] constexpr f128 abs(f128 x) noexcept { return x.hi < 0.0 ? -x : x; }
    [[nodiscard]] constexpr f256 abs(f256 x) noexcept { return x.x[0] < 0.0 ? -x : x; }

    template <class T>
    [[nodiscard]] constexpr T sign(const T &x) noexcept {
        if constexpr (is_unsigned_v<T>) {
            return x == T(0) ? T(0) : T(1);
        } else {
            return x < T(0) ? T(-1) : (T(0) < x ? T(1) : T(0));
        }
    }

    template <floating_point T>
    [[nodiscard]] inline T copysign(T magnitude, T sign_source) noexcept {
        return ::copysign(magnitude, sign_source);
    }
    [[nodiscard]] constexpr f128 copysign(f128 magnitude, f128 sign_source) noexcept {
        return signbit(sign_source) ? -abs(magnitude) : abs(magnitude);
    }
    [[nodiscard]] constexpr f256 copysign(f256 magnitude, const f256 &sign_source) noexcept {
        return signbit(sign_source) ? -abs(magnitude) : abs(magnitude);
    }

    // --- scaling ------------------------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T ldexp(T x, int exp) noexcept {
        return ::ldexp(x, exp);
    }
    [[nodiscard]] constexpr f128 ldexp(f128 x, int exp) noexcept { return f128(Detail::ldexp_f64(x.hi, exp), Detail::ldexp_f64(x.lo, exp)); }
    [[nodiscard]] constexpr f256 ldexp(const f256 &x, int exp) noexcept {
        return f256(Detail::ldexp_f64(x.x[0], exp), Detail::ldexp_f64(x.x[1], exp), Detail::ldexp_f64(x.x[2], exp), Detail::ldexp_f64(x.x[3], exp));
    }
    template <class T>
    [[nodiscard]] inline T scalbn(T x, int exp) noexcept {
        return ldexp(x, exp);
    }

    // --- sqrt / cbrt --------------------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T sqrt(T x) noexcept {
        return ::sqrt(x);
    }
    [[nodiscard]] constexpr f128 sqrt(f128 a) noexcept {
        if (isnan(a))
            return a;
        if (a < f128(0.0))
            return Detail::quiet_nan<f128>();
        if (isinf(a))
            return Detail::infinity<f128>();
        if (a.hi == 0.0 && a.lo == 0.0)
            return f128(0.0);
        // One Newton refinement of a double seed: 53 -> ~106 bits.
        const f64 x = 1.0 / Detail::sqrt_f64(a.hi);
        const f64 ax = a.hi * x;
        const Detail::TwoF64 ax2 = Detail::two_prod(ax, ax);
        const f128 diff = a - f128(ax2.hi, ax2.lo);
        const Detail::TwoF64 s = Detail::two_sum(ax, diff.hi * (x * 0.5));
        return f128(s.hi, s.lo);
    }
    [[nodiscard]] constexpr f256 sqrt(const f256 &a) noexcept {
        if (isnan(a))
            return a;
        if (a < f256(0.0))
            return Detail::quiet_nan<f256>();
        if (isinf(a))
            return Detail::infinity<f256>();
        if (a.x[0] == 0.0)
            return f256(0.0);
        // Newton on the reciprocal square root (no division): each step doubles the digits,
        // so a double seed needs three steps to reach quad-double, then sqrt(a) = a * rsqrt(a).
        const f256 half = f256(0.5);
        const f256 h = a * half;
        f256 y = f256(1.0 / Detail::sqrt_f64(a.x[0]));
        y = y + y * (half - h * (y * y));
        y = y + y * (half - h * (y * y));
        y = y + y * (half - h * (y * y));
        return a * y;
    }

    template <floating_point T>
    [[nodiscard]] inline T cbrt(T x) noexcept {
        return ::cbrt(x);
    }
    [[nodiscard]] constexpr f128 cbrt(f128 a) noexcept {
        if (a == f128(0.0) || !isfinite(a))
            return a;
        f128 y(Detail::cbrt_f64(static_cast<f64>(a)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i)
            y = (f128(2.0) * y + a / (y * y)) / f128(3.0);
        return y;
    }
    [[nodiscard]] constexpr f256 cbrt(const f256 &a) noexcept {
        if (a == f256(0.0) || !isfinite(a))
            return a;
        f256 y(Detail::cbrt_f64(static_cast<f64>(a)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i)
            y = (f256(2.0) * y + a / (y * y)) / f256(3.0);
        return y;
    }

    // --- fma (full-precision a*b + c) ---------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T fma(T a, T b, T c) noexcept {
        return ::fma(a, b, c);
    }
    [[nodiscard]] constexpr f128 fma(f128 a, f128 b, f128 c) noexcept { return a * b + c; }
    [[nodiscard]] constexpr f256 fma(const f256 &a, const f256 &b, const f256 &c) noexcept { return a * b + c; }

    // --- floor / ceil / trunc / round ---------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T floor(T x) noexcept {
        return ::floor(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T ceil(T x) noexcept {
        return ::ceil(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T trunc(T x) noexcept {
        return ::trunc(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T round(T x) noexcept {
        return ::round(x);
    }

    [[nodiscard]] constexpr f128 floor(f128 a) noexcept {
        const f64 hi = Detail::floor_f64(a.hi);
        if (hi != a.hi)
            return f128(hi, 0.0); // fraction lives in the high word
        const Detail::TwoF64 s = Detail::quick_two_sum(hi, Detail::floor_f64(a.lo));
        return f128(s.hi, s.lo);
    }
    [[nodiscard]] constexpr f256 floor(const f256 &a) noexcept {
        f64 x0 = Detail::floor_f64(a.x[0]);
        f64 x1 = 0.0, x2 = 0.0, x3 = 0.0;
        if (x0 == a.x[0]) { // descend into lower words only while higher ones are integer-exact
            x1 = Detail::floor_f64(a.x[1]);
            if (x1 == a.x[1]) {
                x2 = Detail::floor_f64(a.x[2]);
                if (x2 == a.x[2])
                    x3 = Detail::floor_f64(a.x[3]);
            }
            Detail::renorm5(x0, x1, x2, x3, 0.0);
        }
        return f256(x0, x1, x2, x3);
    }

    [[nodiscard]] constexpr f128 ceil(f128 a) noexcept { return -floor(-a); }
    [[nodiscard]] constexpr f256 ceil(const f256 &a) noexcept { return -floor(-a); }
    [[nodiscard]] constexpr f128 trunc(f128 a) noexcept { return a.hi < 0.0 ? ceil(a) : floor(a); }
    [[nodiscard]] constexpr f256 trunc(const f256 &a) noexcept { return a.x[0] < 0.0 ? ceil(a) : floor(a); }
    // round half away from zero, matching ::round.
    [[nodiscard]] constexpr f128 round(f128 a) noexcept { return a.hi < 0.0 ? ceil(a - f128(0.5)) : floor(a + f128(0.5)); }
    [[nodiscard]] constexpr f256 round(const f256 &a) noexcept { return a.x[0] < 0.0 ? ceil(a - f256(0.5)) : floor(a + f256(0.5)); }

    namespace Detail {

        template <WideFloat T>
        [[nodiscard]] constexpr T sin_kernel(T x) noexcept {
            const T x2 = x * x;
            T term = x;
            T sum = x;
            for (int n = 1; n <= series_iterations<T>; ++n) {
                const f64 denom = static_cast<f64>((2 * n) * (2 * n + 1));
                term = -(term * x2) / T(denom);
                const T next = sum + term;
                if (next == sum)
                    return next;
                sum = next;
            }
            return sum;
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T cos_kernel(T x) noexcept {
            const T x2 = x * x;
            T term(1.0);
            T sum(1.0);
            for (int n = 1; n <= series_iterations<T>; ++n) {
                const f64 denom = static_cast<f64>((2 * n - 1) * (2 * n));
                term = -(term * x2) / T(denom);
                const T next = sum + term;
                if (next == sum)
                    return next;
                sum = next;
            }
            return sum;
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T natural_log_two() noexcept {
            return constant<T>(0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56, 0x1.7b57a079a1934p-111, -0x1.ace93a4ebe5d1p-165);
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T exp_kernel(T x) noexcept {
            T term(1.0);
            T sum(1.0);
            for (int n = 1; n <= series_iterations<T>; ++n) {
                term = (term * x) / T(static_cast<f64>(n));
                const T next = sum + term;
                if (next == sum)
                    return next;
                sum = next;
            }
            return sum;
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T reduce_half_pi(T x, int &quadrant) noexcept {
            const T kf = round(x * two_over_pi<T>());
            const f64 kd = static_cast<f64>(kf);
            if (!f64_isfinite(kd) || kd < static_cast<f64>(numeric_limits<i64>::min()) ||
                kd > static_cast<f64>(numeric_limits<i64>::max())) {
                quadrant = 0;
                return T(remainder_f64(static_cast<f64>(x), half_pi<f64>()));
            }

            const i64 k = static_cast<i64>(kf);
            int q = static_cast<int>(k % 4);
            if (q < 0)
                q += 4;
            quadrant = q;
            return x - T(static_cast<f64>(k)) * half_pi<T>();
        }

        template <WideFloat T>
        [[nodiscard]] constexpr T powi(T base, i64 exponent) noexcept {
            if (exponent == 0)
                return T(1.0);
            const bool invert = exponent < 0;
            u64 n = invert ? static_cast<u64>(-(exponent + 1)) + 1u : static_cast<u64>(exponent);
            T result(1.0);
            while (n != 0) {
                if ((n & 1u) != 0)
                    result = result * base;
                base = base * base;
                n >>= 1u;
            }
            return invert ? T(1.0) / result : result;
        }

        template <WideFloat T>
        [[nodiscard]] constexpr bool integral_exponent(T x, i64 &out) noexcept {
            const T rounded = trunc(x);
            if (!(rounded == x))
                return false;
            const f64 xd = static_cast<f64>(x);
            if (!f64_isfinite(xd) || xd < static_cast<f64>(numeric_limits<i64>::min()) ||
                xd > static_cast<f64>(numeric_limits<i64>::max()))
                return false;
            out = static_cast<i64>(x);
            return true;
        }

    } // namespace Detail

    // --- trigonometry -------------------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T sin(T x) noexcept {
        return ::sin(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T cos(T x) noexcept {
        return ::cos(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T tan(T x) noexcept {
        return ::tan(x);
    }

    [[nodiscard]] constexpr f128 sin(f128 x) noexcept {
        if (!isfinite(x))
            return Detail::quiet_nan<f128>();
        int quadrant = 0;
        const f128 r = Detail::reduce_half_pi(x, quadrant);
        switch (quadrant) {
            case 0:
                return Detail::sin_kernel(r);
            case 1:
                return Detail::cos_kernel(r);
            case 2:
                return -Detail::sin_kernel(r);
            default:
                return -Detail::cos_kernel(r);
        }
    }
    [[nodiscard]] constexpr f128 cos(f128 x) noexcept {
        if (!isfinite(x))
            return Detail::quiet_nan<f128>();
        int quadrant = 0;
        const f128 r = Detail::reduce_half_pi(x, quadrant);
        switch (quadrant) {
            case 0:
                return Detail::cos_kernel(r);
            case 1:
                return -Detail::sin_kernel(r);
            case 2:
                return -Detail::cos_kernel(r);
            default:
                return Detail::sin_kernel(r);
        }
    }
    [[nodiscard]] constexpr f128 tan(f128 x) noexcept { return sin(x) / cos(x); }

    [[nodiscard]] constexpr f256 sin(const f256 &x) noexcept {
        if (!isfinite(x))
            return Detail::quiet_nan<f256>();
        int quadrant = 0;
        const f256 r = Detail::reduce_half_pi(x, quadrant);
        switch (quadrant) {
            case 0:
                return Detail::sin_kernel(r);
            case 1:
                return Detail::cos_kernel(r);
            case 2:
                return -Detail::sin_kernel(r);
            default:
                return -Detail::cos_kernel(r);
        }
    }
    [[nodiscard]] constexpr f256 cos(const f256 &x) noexcept {
        if (!isfinite(x))
            return Detail::quiet_nan<f256>();
        int quadrant = 0;
        const f256 r = Detail::reduce_half_pi(x, quadrant);
        switch (quadrant) {
            case 0:
                return Detail::cos_kernel(r);
            case 1:
                return -Detail::sin_kernel(r);
            case 2:
                return -Detail::cos_kernel(r);
            default:
                return Detail::sin_kernel(r);
        }
    }
    [[nodiscard]] constexpr f256 tan(const f256 &x) noexcept { return sin(x) / cos(x); }

    // --- exponentials and logarithms ----------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T exp(T x) noexcept {
        return ::exp(x);
    }
    [[nodiscard]] constexpr f128 exp(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? f128(0.0) : Detail::infinity<f128>();

        const f128 kf = round(x * log2_e<f128>());
        const f64 kd = static_cast<f64>(kf);
        if (kd > static_cast<f64>(numeric_limits<int>::max()))
            return Detail::infinity<f128>();
        if (kd < static_cast<f64>(numeric_limits<int>::min()))
            return f128(0.0);
        const int k = static_cast<int>(static_cast<i64>(kf));
        const f128 r = x - f128(static_cast<f64>(k)) * Detail::natural_log_two<f128>();
        return ldexp(Detail::exp_kernel(r), k);
    }
    [[nodiscard]] constexpr f256 exp(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? f256(0.0) : Detail::infinity<f256>();

        const f256 kf = round(x * log2_e<f256>());
        const f64 kd = static_cast<f64>(kf);
        if (kd > static_cast<f64>(numeric_limits<int>::max()))
            return Detail::infinity<f256>();
        if (kd < static_cast<f64>(numeric_limits<int>::min()))
            return f256(0.0);
        const int k = static_cast<int>(static_cast<i64>(kf));
        const f256 r = x - f256(static_cast<f64>(k)) * Detail::natural_log_two<f256>();
        return ldexp(Detail::exp_kernel(r), k);
    }

    template <floating_point T>
    [[nodiscard]] inline T log(T x) noexcept {
        return ::log(x);
    }
    [[nodiscard]] constexpr f128 log(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (x < f128(0.0))
            return Detail::quiet_nan<f128>();
        if (x == f128(0.0))
            return Detail::negative_infinity<f128>();
        if (isinf(x))
            return Detail::infinity<f128>();

        f128 y(Detail::log_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i) {
            const f128 ey = exp(y);
            y = y + f128(2.0) * (x - ey) / (x + ey);
        }
        return y;
    }
    [[nodiscard]] constexpr f256 log(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (x < f256(0.0))
            return Detail::quiet_nan<f256>();
        if (x == f256(0.0))
            return Detail::negative_infinity<f256>();
        if (isinf(x))
            return Detail::infinity<f256>();

        f256 y(Detail::log_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i) {
            const f256 ey = exp(y);
            y = y + f256(2.0) * (x - ey) / (x + ey);
        }
        return y;
    }

    template <floating_point T>
    [[nodiscard]] inline T log2(T x) noexcept {
        return ::log2(x);
    }
    [[nodiscard]] constexpr f128 log2(f128 x) noexcept { return log(x) * log2_e<f128>(); }
    [[nodiscard]] constexpr f256 log2(const f256 &x) noexcept { return log(x) * log2_e<f256>(); }

    template <floating_point T>
    [[nodiscard]] inline T log10(T x) noexcept {
        return ::log10(x);
    }
    [[nodiscard]] constexpr f128 log10(f128 x) noexcept { return log(x) * log10_e<f128>(); }
    [[nodiscard]] constexpr f256 log10(const f256 &x) noexcept { return log(x) * log10_e<f256>(); }

    template <floating_point T>
    [[nodiscard]] inline T pow(T base, T exponent) noexcept {
        return ::pow(base, exponent);
    }
    [[nodiscard]] constexpr f128 pow(f128 base, f128 exponent) noexcept {
        i64 integral = 0;
        if (Detail::integral_exponent(exponent, integral))
            return Detail::powi(base, integral);
        if (base < f128(0.0))
            return Detail::quiet_nan<f128>();
        return exp(exponent * log(base));
    }
    [[nodiscard]] constexpr f256 pow(const f256 &base, const f256 &exponent) noexcept {
        i64 integral = 0;
        if (Detail::integral_exponent(exponent, integral))
            return Detail::powi(base, integral);
        if (base < f256(0.0))
            return Detail::quiet_nan<f256>();
        return exp(exponent * log(base));
    }

    // --- inverse trigonometry -----------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T asin(T x) noexcept {
        return ::asin(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T acos(T x) noexcept {
        return ::acos(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T atan(T x) noexcept {
        return ::atan(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T atan2(T y, T x) noexcept {
        return ::atan2(y, x);
    }

    [[nodiscard]] constexpr f128 asin(f128 x) noexcept {
        if (x == f128(1.0))
            return half_pi<f128>();
        if (x == f128(-1.0))
            return -half_pi<f128>();
        if (x < f128(-1.0) || x > f128(1.0))
            return Detail::quiet_nan<f128>();
        f128 y(Detail::asin_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i)
            y = y - (sin(y) - x) / cos(y);
        return y;
    }
    [[nodiscard]] constexpr f256 asin(const f256 &x) noexcept {
        if (x == f256(1.0))
            return half_pi<f256>();
        if (x == f256(-1.0))
            return -half_pi<f256>();
        if (x < f256(-1.0) || x > f256(1.0))
            return Detail::quiet_nan<f256>();
        f256 y(Detail::asin_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i)
            y = y - (sin(y) - x) / cos(y);
        return y;
    }

    [[nodiscard]] constexpr f128 acos(f128 x) noexcept { return half_pi<f128>() - asin(x); }
    [[nodiscard]] constexpr f256 acos(const f256 &x) noexcept { return half_pi<f256>() - asin(x); }

    [[nodiscard]] constexpr f128 atan(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? -half_pi<f128>() : half_pi<f128>();
        f128 y(Detail::atan_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i) {
            const f128 t = tan(y);
            y = y - (t - x) / (f128(1.0) + t * t);
        }
        return y;
    }
    [[nodiscard]] constexpr f256 atan(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? -half_pi<f256>() : half_pi<f256>();
        f256 y(Detail::atan_f64(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i) {
            const f256 t = tan(y);
            y = y - (t - x) / (f256(1.0) + t * t);
        }
        return y;
    }

    [[nodiscard]] constexpr f128 atan2(f128 y, f128 x) noexcept {
        if (isnan(y) || isnan(x))
            return Detail::quiet_nan<f128>();
        if (x > f128(0.0))
            return atan(y / x);
        if (x < f128(0.0))
            return y >= f128(0.0) ? atan(y / x) + pi<f128>() : atan(y / x) - pi<f128>();
        if (y > f128(0.0))
            return half_pi<f128>();
        if (y < f128(0.0))
            return -half_pi<f128>();
        return f128(0.0);
    }
    [[nodiscard]] constexpr f256 atan2(const f256 &y, const f256 &x) noexcept {
        if (isnan(y) || isnan(x))
            return Detail::quiet_nan<f256>();
        if (x > f256(0.0))
            return atan(y / x);
        if (x < f256(0.0))
            return y >= f256(0.0) ? atan(y / x) + pi<f256>() : atan(y / x) - pi<f256>();
        if (y > f256(0.0))
            return half_pi<f256>();
        if (y < f256(0.0))
            return -half_pi<f256>();
        return f256(0.0);
    }

    // --- hyperbolic ---------------------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T sinh(T x) noexcept {
        return ::sinh(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T cosh(T x) noexcept {
        return ::cosh(x);
    }
    template <floating_point T>
    [[nodiscard]] inline T tanh(T x) noexcept {
        return ::tanh(x);
    }
    [[nodiscard]] constexpr f128 sinh(f128 x) noexcept {
        const f128 ex = exp(x);
        const f128 enx = exp(-x);
        return (ex - enx) * f128(0.5);
    }
    [[nodiscard]] constexpr f128 cosh(f128 x) noexcept {
        const f128 ex = exp(x);
        const f128 enx = exp(-x);
        return (ex + enx) * f128(0.5);
    }
    [[nodiscard]] constexpr f128 tanh(f128 x) noexcept {
        const f128 ex2 = exp(f128(2.0) * x);
        return (ex2 - f128(1.0)) / (ex2 + f128(1.0));
    }
    [[nodiscard]] constexpr f256 sinh(const f256 &x) noexcept {
        const f256 ex = exp(x);
        const f256 enx = exp(-x);
        return (ex - enx) * f256(0.5);
    }
    [[nodiscard]] constexpr f256 cosh(const f256 &x) noexcept {
        const f256 ex = exp(x);
        const f256 enx = exp(-x);
        return (ex + enx) * f256(0.5);
    }
    [[nodiscard]] constexpr f256 tanh(const f256 &x) noexcept {
        const f256 ex2 = exp(f256(2.0) * x);
        return (ex2 - f256(1.0)) / (ex2 + f256(1.0));
    }

    // --- common numeric helpers ---------------------------------------------------------------
    template <floating_point T>
    [[nodiscard]] inline T fmod(T x, T y) noexcept {
        return ::fmod(x, y);
    }
    [[nodiscard]] constexpr f128 fmod(f128 x, f128 y) noexcept {
        if (y == f128(0.0))
            return Detail::quiet_nan<f128>();
        return x - trunc(x / y) * y;
    }
    [[nodiscard]] constexpr f256 fmod(const f256 &x, const f256 &y) noexcept {
        if (y == f256(0.0))
            return Detail::quiet_nan<f256>();
        return x - trunc(x / y) * y;
    }

    template <floating_point T>
    [[nodiscard]] inline T remainder(T x, T y) noexcept {
        return ::remainder(x, y);
    }
    [[nodiscard]] constexpr f128 remainder(f128 x, f128 y) noexcept {
        if (y == f128(0.0))
            return Detail::quiet_nan<f128>();
        return x - round(x / y) * y;
    }
    [[nodiscard]] constexpr f256 remainder(const f256 &x, const f256 &y) noexcept {
        if (y == f256(0.0))
            return Detail::quiet_nan<f256>();
        return x - round(x / y) * y;
    }

    template <floating_point T>
    [[nodiscard]] inline T hypot(T x, T y) noexcept {
        return ::hypot(x, y);
    }
    [[nodiscard]] constexpr f128 hypot(f128 x, f128 y) noexcept { return sqrt(x * x + y * y); }
    [[nodiscard]] constexpr f256 hypot(const f256 &x, const f256 &y) noexcept { return sqrt(x * x + y * y); }

    template <floating_point T>
    [[nodiscard]] inline T lerp(T a, T b, T t) noexcept {
        return ::lerp(a, b, t);
    }
    [[nodiscard]] constexpr f128 lerp(f128 a, f128 b, f128 t) noexcept { return a + (b - a) * t; }
    [[nodiscard]] constexpr f256 lerp(const f256 &a, const f256 &b, const f256 &t) noexcept { return a + (b - a) * t; }

    template <floating_point T>
    [[nodiscard]] inline T fract(T x) noexcept {
        return x - floor(x);
    }
    [[nodiscard]] constexpr f128 fract(f128 x) noexcept { return x - floor(x); }
    [[nodiscard]] constexpr f256 fract(const f256 &x) noexcept { return x - floor(x); }

    template <floating_point T>
    [[nodiscard]] inline T saturate(T x) noexcept {
        return x < T(0) ? T(0) : (x > T(1) ? T(1) : x);
    }
    [[nodiscard]] constexpr f128 saturate(f128 x) noexcept { return x < f128(0.0) ? f128(0.0) : (x > f128(1.0) ? f128(1.0) : x); }
    [[nodiscard]] constexpr f256 saturate(const f256 &x) noexcept { return x < f256(0.0) ? f256(0.0) : (x > f256(1.0) ? f256(1.0) : x); }

    namespace Detail {

        [[nodiscard]] consteval bool wide_math_constexpr_smoke_test() noexcept {
            constexpr f128 root128 = SFT::Foundation::sqrt(f128(4.0));
            if (static_cast<f64>(root128) != 2.0)
                return false;

            constexpr f256 root256 = SFT::Foundation::sqrt(f256(9.0));
            if (static_cast<f64>(root256) != 3.0)
                return false;

            constexpr f128 sine_zero = SFT::Foundation::sin(f128(0.0));
            if (static_cast<f64>(sine_zero) != 0.0)
                return false;

            constexpr f256 cosine_zero = SFT::Foundation::cos(f256(0.0));
            if (static_cast<f64>(cosine_zero) != 1.0)
                return false;

            constexpr f128 exp_zero = SFT::Foundation::exp(f128(0.0));
            if (static_cast<f64>(exp_zero) != 1.0)
                return false;

            constexpr f128 log_one = SFT::Foundation::log(f128(1.0));
            if (static_cast<f64>(log_one) != 0.0)
                return false;

            constexpr f256 pow_value = SFT::Foundation::pow(f256(2.0), f256(3.0));
            return static_cast<f64>(pow_value) == 8.0;
        }

        static_assert(wide_math_constexpr_smoke_test());

    } // namespace Detail

} // namespace SFT::Foundation
