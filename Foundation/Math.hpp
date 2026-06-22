#pragma once

#include "Foundation/Constants.hpp"
#include "Foundation/Types.hpp"
#include "Foundation/Wide.hpp"

#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>

// Math functions that work uniformly across the regular scalars and the wide types.
//
// The contract has two halves:
//   * Built-in scalars forward straight to <cmath>. These wrappers are intentionally thin and inline
//     to the same operations a direct std::sqrt/std::sin/std::log call would use.
//   * Wide types (i128/u128/i256/u256/f128/f256) get explicit overloads so generic engine code can
//     opt into extra precision without silently falling back to f64.
//
// f128/f256 are expansion floats (double-double / quad-double). Their transcendental functions use
// standard range reduction plus Newton/Taylor refinement in the wide type. They prioritize predictable
// full-width behavior over libm-level platform-specific tricks; for built-ins, the platform libm remains
// the fastest path.

namespace SFT::Foundation {

    namespace Detail {

        template <class T>
        concept WideFloat = std::same_as<std::remove_cvref_t<T>, f128> || std::same_as<std::remove_cvref_t<T>, f256>;

        template <class T>
        concept WideNumber = std::same_as<std::remove_cvref_t<T>, u256> || std::same_as<std::remove_cvref_t<T>, i256> || WideFloat<T>;

        template <WideFloat T>
        inline constexpr int series_iterations = std::same_as<std::remove_cvref_t<T>, f128> ? 64 : 128;

        template <WideFloat T>
        inline constexpr int refinement_iterations = std::same_as<std::remove_cvref_t<T>, f128> ? 3 : 5;

        template <WideFloat T>
        [[nodiscard]] inline T quiet_nan() noexcept {
            return T(std::numeric_limits<f64>::quiet_NaN());
        }

        template <WideFloat T>
        [[nodiscard]] inline T infinity() noexcept {
            return T(std::numeric_limits<f64>::infinity());
        }

        template <WideFloat T>
        [[nodiscard]] inline T negative_infinity() noexcept {
            return -infinity<T>();
        }

    } // namespace Detail

    // --- classification -----------------------------------------------------------------------
    template <std::integral T>
    [[nodiscard]] constexpr bool isnan(T) noexcept {
        return false;
    }
    template <std::floating_point T>
    [[nodiscard]] inline bool isnan(T x) noexcept {
        return std::isnan(x);
    }
    [[nodiscard]] constexpr bool isnan(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool isnan(i256) noexcept { return false; }
    [[nodiscard]] inline bool isnan(f128 x) noexcept { return std::isnan(x.hi) || std::isnan(x.lo); }
    [[nodiscard]] inline bool isnan(const f256 &x) noexcept {
        return std::isnan(x.x[0]) || std::isnan(x.x[1]) || std::isnan(x.x[2]) || std::isnan(x.x[3]);
    }

    template <std::integral T>
    [[nodiscard]] constexpr bool isinf(T) noexcept {
        return false;
    }
    template <std::floating_point T>
    [[nodiscard]] inline bool isinf(T x) noexcept {
        return std::isinf(x);
    }
    [[nodiscard]] constexpr bool isinf(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool isinf(i256) noexcept { return false; }
    [[nodiscard]] inline bool isinf(f128 x) noexcept { return std::isinf(x.hi); }
    [[nodiscard]] inline bool isinf(const f256 &x) noexcept { return std::isinf(x.x[0]); }

    template <std::integral T>
    [[nodiscard]] constexpr bool isfinite(T) noexcept {
        return true;
    }
    template <std::floating_point T>
    [[nodiscard]] inline bool isfinite(T x) noexcept {
        return std::isfinite(x);
    }
    [[nodiscard]] constexpr bool isfinite(u256) noexcept { return true; }
    [[nodiscard]] constexpr bool isfinite(i256) noexcept { return true; }
    [[nodiscard]] inline bool isfinite(f128 x) noexcept { return std::isfinite(x.hi) && std::isfinite(x.lo); }
    [[nodiscard]] inline bool isfinite(const f256 &x) noexcept {
        return std::isfinite(x.x[0]) && std::isfinite(x.x[1]) && std::isfinite(x.x[2]) && std::isfinite(x.x[3]);
    }

    template <std::integral T>
    [[nodiscard]] constexpr bool signbit(T x) noexcept {
        if constexpr (std::is_unsigned_v<T>)
            return false;
        else
            return x < 0;
    }
    template <std::floating_point T>
    [[nodiscard]] inline bool signbit(T x) noexcept {
        return std::signbit(x);
    }
    [[nodiscard]] constexpr bool signbit(u256) noexcept { return false; }
    [[nodiscard]] constexpr bool signbit(i256 x) noexcept { return x.is_negative(); }
    [[nodiscard]] inline bool signbit(f128 x) noexcept { return std::signbit(x.hi) || (x.hi == 0.0 && std::signbit(x.lo)); }
    [[nodiscard]] inline bool signbit(const f256 &x) noexcept {
        return std::signbit(x.x[0]) || (x.x[0] == 0.0 && (std::signbit(x.x[1]) || std::signbit(x.x[2]) || std::signbit(x.x[3])));
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
    template <std::floating_point T>
    [[nodiscard]] inline T abs(T x) noexcept {
        return std::fabs(x);
    }
    template <std::integral T>
    [[nodiscard]] constexpr T abs(T x) noexcept {
        if constexpr (std::is_unsigned_v<T>) {
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
        if constexpr (std::is_unsigned_v<T>) {
            return x == T(0) ? T(0) : T(1);
        } else {
            return x < T(0) ? T(-1) : (T(0) < x ? T(1) : T(0));
        }
    }

    template <std::floating_point T>
    [[nodiscard]] inline T copysign(T magnitude, T sign_source) noexcept {
        return std::copysign(magnitude, sign_source);
    }
    [[nodiscard]] inline f128 copysign(f128 magnitude, f128 sign_source) noexcept {
        return signbit(sign_source) ? -abs(magnitude) : abs(magnitude);
    }
    [[nodiscard]] inline f256 copysign(f256 magnitude, const f256 &sign_source) noexcept {
        return signbit(sign_source) ? -abs(magnitude) : abs(magnitude);
    }

    // --- scaling ------------------------------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T ldexp(T x, int exp) noexcept {
        return std::ldexp(x, exp);
    }
    [[nodiscard]] inline f128 ldexp(f128 x, int exp) noexcept { return f128(std::ldexp(x.hi, exp), std::ldexp(x.lo, exp)); }
    [[nodiscard]] inline f256 ldexp(const f256 &x, int exp) noexcept {
        return f256(std::ldexp(x.x[0], exp), std::ldexp(x.x[1], exp), std::ldexp(x.x[2], exp), std::ldexp(x.x[3], exp));
    }
    template <class T>
    [[nodiscard]] inline T scalbn(T x, int exp) noexcept {
        return ldexp(x, exp);
    }

    // --- sqrt / cbrt --------------------------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T sqrt(T x) noexcept {
        return std::sqrt(x);
    }
    [[nodiscard]] inline f128 sqrt(f128 a) noexcept {
        if (isnan(a))
            return a;
        if (a < f128(0.0))
            return Detail::quiet_nan<f128>();
        if (isinf(a))
            return Detail::infinity<f128>();
        if (a.hi == 0.0 && a.lo == 0.0)
            return f128(0.0);
        // One Newton refinement of a double seed: 53 -> ~106 bits.
        const f64 x = 1.0 / std::sqrt(a.hi);
        const f64 ax = a.hi * x;
        const Detail::TwoF64 ax2 = Detail::two_prod(ax, ax);
        const f128 diff = a - f128(ax2.hi, ax2.lo);
        const Detail::TwoF64 s = Detail::two_sum(ax, diff.hi * (x * 0.5));
        return f128(s.hi, s.lo);
    }
    [[nodiscard]] inline f256 sqrt(const f256 &a) noexcept {
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
        f256 y = f256(1.0 / std::sqrt(a.x[0]));
        y = y + y * (half - h * (y * y));
        y = y + y * (half - h * (y * y));
        y = y + y * (half - h * (y * y));
        return a * y;
    }

    template <std::floating_point T>
    [[nodiscard]] inline T cbrt(T x) noexcept {
        return std::cbrt(x);
    }
    [[nodiscard]] inline f128 cbrt(f128 a) noexcept {
        if (a == f128(0.0) || !isfinite(a))
            return a;
        f128 y(std::cbrt(static_cast<f64>(a)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i)
            y = (f128(2.0) * y + a / (y * y)) / f128(3.0);
        return y;
    }
    [[nodiscard]] inline f256 cbrt(const f256 &a) noexcept {
        if (a == f256(0.0) || !isfinite(a))
            return a;
        f256 y(std::cbrt(static_cast<f64>(a)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i)
            y = (f256(2.0) * y + a / (y * y)) / f256(3.0);
        return y;
    }

    // --- fma (full-precision a*b + c) ---------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T fma(T a, T b, T c) noexcept {
        return std::fma(a, b, c);
    }
    [[nodiscard]] inline f128 fma(f128 a, f128 b, f128 c) noexcept { return a * b + c; }
    [[nodiscard]] inline f256 fma(const f256 &a, const f256 &b, const f256 &c) noexcept { return a * b + c; }

    // --- floor / ceil / trunc / round ---------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T floor(T x) noexcept {
        return std::floor(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T ceil(T x) noexcept {
        return std::ceil(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T trunc(T x) noexcept {
        return std::trunc(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T round(T x) noexcept {
        return std::round(x);
    }

    [[nodiscard]] inline f128 floor(f128 a) noexcept {
        const f64 hi = std::floor(a.hi);
        if (hi != a.hi)
            return f128(hi, 0.0); // fraction lives in the high word
        const Detail::TwoF64 s = Detail::quick_two_sum(hi, std::floor(a.lo));
        return f128(s.hi, s.lo);
    }
    [[nodiscard]] inline f256 floor(const f256 &a) noexcept {
        f64 x0 = std::floor(a.x[0]);
        f64 x1 = 0.0, x2 = 0.0, x3 = 0.0;
        if (x0 == a.x[0]) { // descend into lower words only while higher ones are integer-exact
            x1 = std::floor(a.x[1]);
            if (x1 == a.x[1]) {
                x2 = std::floor(a.x[2]);
                if (x2 == a.x[2])
                    x3 = std::floor(a.x[3]);
            }
            Detail::renorm5(x0, x1, x2, x3, 0.0);
        }
        return f256(x0, x1, x2, x3);
    }

    [[nodiscard]] inline f128 ceil(f128 a) noexcept { return -floor(-a); }
    [[nodiscard]] inline f256 ceil(const f256 &a) noexcept { return -floor(-a); }
    [[nodiscard]] inline f128 trunc(f128 a) noexcept { return a.hi < 0.0 ? ceil(a) : floor(a); }
    [[nodiscard]] inline f256 trunc(const f256 &a) noexcept { return a.x[0] < 0.0 ? ceil(a) : floor(a); }
    // round half away from zero, matching std::round.
    [[nodiscard]] inline f128 round(f128 a) noexcept { return a.hi < 0.0 ? ceil(a - f128(0.5)) : floor(a + f128(0.5)); }
    [[nodiscard]] inline f256 round(const f256 &a) noexcept { return a.x[0] < 0.0 ? ceil(a - f256(0.5)) : floor(a + f256(0.5)); }

    namespace Detail {

        template <WideFloat T>
        [[nodiscard]] inline T sin_kernel(T x) noexcept {
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
        [[nodiscard]] inline T cos_kernel(T x) noexcept {
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
        [[nodiscard]] inline T exp_kernel(T x) noexcept {
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
        [[nodiscard]] inline T reduce_half_pi(T x, int &quadrant) noexcept {
            const T kf = round(x * two_over_pi<T>());
            const f64 kd = static_cast<f64>(kf);
            if (!std::isfinite(kd) || kd < static_cast<f64>(std::numeric_limits<i64>::min()) ||
                kd > static_cast<f64>(std::numeric_limits<i64>::max())) {
                quadrant = 0;
                return T(std::remainder(static_cast<f64>(x), half_pi<f64>()));
            }

            const i64 k = static_cast<i64>(kf);
            int q = static_cast<int>(k % 4);
            if (q < 0)
                q += 4;
            quadrant = q;
            return x - T(static_cast<f64>(k)) * half_pi<T>();
        }

        template <WideFloat T>
        [[nodiscard]] inline T powi(T base, i64 exponent) noexcept {
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
        [[nodiscard]] inline bool integral_exponent(T x, i64 &out) noexcept {
            const T rounded = trunc(x);
            if (!(rounded == x))
                return false;
            const f64 xd = static_cast<f64>(x);
            if (!std::isfinite(xd) || xd < static_cast<f64>(std::numeric_limits<i64>::min()) ||
                xd > static_cast<f64>(std::numeric_limits<i64>::max()))
                return false;
            out = static_cast<i64>(x);
            return true;
        }

    } // namespace Detail

    // --- trigonometry -------------------------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T sin(T x) noexcept {
        return std::sin(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T cos(T x) noexcept {
        return std::cos(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T tan(T x) noexcept {
        return std::tan(x);
    }

    [[nodiscard]] inline f128 sin(f128 x) noexcept {
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
    [[nodiscard]] inline f128 cos(f128 x) noexcept {
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
    [[nodiscard]] inline f128 tan(f128 x) noexcept { return sin(x) / cos(x); }

    [[nodiscard]] inline f256 sin(const f256 &x) noexcept {
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
    [[nodiscard]] inline f256 cos(const f256 &x) noexcept {
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
    [[nodiscard]] inline f256 tan(const f256 &x) noexcept { return sin(x) / cos(x); }

    // --- exponentials and logarithms ----------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T exp(T x) noexcept {
        return std::exp(x);
    }
    [[nodiscard]] inline f128 exp(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? f128(0.0) : Detail::infinity<f128>();

        const f128 kf = round(x * log2_e<f128>());
        const f64 kd = static_cast<f64>(kf);
        if (kd > static_cast<f64>(std::numeric_limits<int>::max()))
            return Detail::infinity<f128>();
        if (kd < static_cast<f64>(std::numeric_limits<int>::min()))
            return f128(0.0);
        const int k = static_cast<int>(static_cast<i64>(kf));
        const f128 r = x - f128(static_cast<f64>(k)) * Detail::natural_log_two<f128>();
        return ldexp(Detail::exp_kernel(r), k);
    }
    [[nodiscard]] inline f256 exp(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? f256(0.0) : Detail::infinity<f256>();

        const f256 kf = round(x * log2_e<f256>());
        const f64 kd = static_cast<f64>(kf);
        if (kd > static_cast<f64>(std::numeric_limits<int>::max()))
            return Detail::infinity<f256>();
        if (kd < static_cast<f64>(std::numeric_limits<int>::min()))
            return f256(0.0);
        const int k = static_cast<int>(static_cast<i64>(kf));
        const f256 r = x - f256(static_cast<f64>(k)) * Detail::natural_log_two<f256>();
        return ldexp(Detail::exp_kernel(r), k);
    }

    template <std::floating_point T>
    [[nodiscard]] inline T log(T x) noexcept {
        return std::log(x);
    }
    [[nodiscard]] inline f128 log(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (x < f128(0.0))
            return Detail::quiet_nan<f128>();
        if (x == f128(0.0))
            return Detail::negative_infinity<f128>();
        if (isinf(x))
            return Detail::infinity<f128>();

        f128 y(std::log(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i) {
            const f128 ey = exp(y);
            y = y + f128(2.0) * (x - ey) / (x + ey);
        }
        return y;
    }
    [[nodiscard]] inline f256 log(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (x < f256(0.0))
            return Detail::quiet_nan<f256>();
        if (x == f256(0.0))
            return Detail::negative_infinity<f256>();
        if (isinf(x))
            return Detail::infinity<f256>();

        f256 y(std::log(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i) {
            const f256 ey = exp(y);
            y = y + f256(2.0) * (x - ey) / (x + ey);
        }
        return y;
    }

    template <std::floating_point T>
    [[nodiscard]] inline T log2(T x) noexcept {
        return std::log2(x);
    }
    [[nodiscard]] inline f128 log2(f128 x) noexcept { return log(x) * log2_e<f128>(); }
    [[nodiscard]] inline f256 log2(const f256 &x) noexcept { return log(x) * log2_e<f256>(); }

    template <std::floating_point T>
    [[nodiscard]] inline T log10(T x) noexcept {
        return std::log10(x);
    }
    [[nodiscard]] inline f128 log10(f128 x) noexcept { return log(x) * log10_e<f128>(); }
    [[nodiscard]] inline f256 log10(const f256 &x) noexcept { return log(x) * log10_e<f256>(); }

    template <std::floating_point T>
    [[nodiscard]] inline T pow(T base, T exponent) noexcept {
        return std::pow(base, exponent);
    }
    [[nodiscard]] inline f128 pow(f128 base, f128 exponent) noexcept {
        i64 integral = 0;
        if (Detail::integral_exponent(exponent, integral))
            return Detail::powi(base, integral);
        if (base < f128(0.0))
            return Detail::quiet_nan<f128>();
        return exp(exponent * log(base));
    }
    [[nodiscard]] inline f256 pow(const f256 &base, const f256 &exponent) noexcept {
        i64 integral = 0;
        if (Detail::integral_exponent(exponent, integral))
            return Detail::powi(base, integral);
        if (base < f256(0.0))
            return Detail::quiet_nan<f256>();
        return exp(exponent * log(base));
    }

    // --- inverse trigonometry -----------------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T asin(T x) noexcept {
        return std::asin(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T acos(T x) noexcept {
        return std::acos(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T atan(T x) noexcept {
        return std::atan(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T atan2(T y, T x) noexcept {
        return std::atan2(y, x);
    }

    [[nodiscard]] inline f128 asin(f128 x) noexcept {
        if (x == f128(1.0))
            return half_pi<f128>();
        if (x == f128(-1.0))
            return -half_pi<f128>();
        if (x < f128(-1.0) || x > f128(1.0))
            return Detail::quiet_nan<f128>();
        f128 y(std::asin(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i)
            y = y - (sin(y) - x) / cos(y);
        return y;
    }
    [[nodiscard]] inline f256 asin(const f256 &x) noexcept {
        if (x == f256(1.0))
            return half_pi<f256>();
        if (x == f256(-1.0))
            return -half_pi<f256>();
        if (x < f256(-1.0) || x > f256(1.0))
            return Detail::quiet_nan<f256>();
        f256 y(std::asin(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i)
            y = y - (sin(y) - x) / cos(y);
        return y;
    }

    [[nodiscard]] inline f128 acos(f128 x) noexcept { return half_pi<f128>() - asin(x); }
    [[nodiscard]] inline f256 acos(const f256 &x) noexcept { return half_pi<f256>() - asin(x); }

    [[nodiscard]] inline f128 atan(f128 x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? -half_pi<f128>() : half_pi<f128>();
        f128 y(std::atan(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f128>; ++i) {
            const f128 t = tan(y);
            y = y - (t - x) / (f128(1.0) + t * t);
        }
        return y;
    }
    [[nodiscard]] inline f256 atan(const f256 &x) noexcept {
        if (isnan(x))
            return x;
        if (isinf(x))
            return signbit(x) ? -half_pi<f256>() : half_pi<f256>();
        f256 y(std::atan(static_cast<f64>(x)));
        for (int i = 0; i < Detail::refinement_iterations<f256>; ++i) {
            const f256 t = tan(y);
            y = y - (t - x) / (f256(1.0) + t * t);
        }
        return y;
    }

    [[nodiscard]] inline f128 atan2(f128 y, f128 x) noexcept {
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
    [[nodiscard]] inline f256 atan2(const f256 &y, const f256 &x) noexcept {
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
    template <std::floating_point T>
    [[nodiscard]] inline T sinh(T x) noexcept {
        return std::sinh(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T cosh(T x) noexcept {
        return std::cosh(x);
    }
    template <std::floating_point T>
    [[nodiscard]] inline T tanh(T x) noexcept {
        return std::tanh(x);
    }
    [[nodiscard]] inline f128 sinh(f128 x) noexcept {
        const f128 ex = exp(x);
        const f128 enx = exp(-x);
        return (ex - enx) * f128(0.5);
    }
    [[nodiscard]] inline f128 cosh(f128 x) noexcept {
        const f128 ex = exp(x);
        const f128 enx = exp(-x);
        return (ex + enx) * f128(0.5);
    }
    [[nodiscard]] inline f128 tanh(f128 x) noexcept {
        const f128 ex2 = exp(f128(2.0) * x);
        return (ex2 - f128(1.0)) / (ex2 + f128(1.0));
    }
    [[nodiscard]] inline f256 sinh(const f256 &x) noexcept {
        const f256 ex = exp(x);
        const f256 enx = exp(-x);
        return (ex - enx) * f256(0.5);
    }
    [[nodiscard]] inline f256 cosh(const f256 &x) noexcept {
        const f256 ex = exp(x);
        const f256 enx = exp(-x);
        return (ex + enx) * f256(0.5);
    }
    [[nodiscard]] inline f256 tanh(const f256 &x) noexcept {
        const f256 ex2 = exp(f256(2.0) * x);
        return (ex2 - f256(1.0)) / (ex2 + f256(1.0));
    }

    // --- common numeric helpers ---------------------------------------------------------------
    template <std::floating_point T>
    [[nodiscard]] inline T fmod(T x, T y) noexcept {
        return std::fmod(x, y);
    }
    [[nodiscard]] inline f128 fmod(f128 x, f128 y) noexcept {
        if (y == f128(0.0))
            return Detail::quiet_nan<f128>();
        return x - trunc(x / y) * y;
    }
    [[nodiscard]] inline f256 fmod(const f256 &x, const f256 &y) noexcept {
        if (y == f256(0.0))
            return Detail::quiet_nan<f256>();
        return x - trunc(x / y) * y;
    }

    template <std::floating_point T>
    [[nodiscard]] inline T remainder(T x, T y) noexcept {
        return std::remainder(x, y);
    }
    [[nodiscard]] inline f128 remainder(f128 x, f128 y) noexcept {
        if (y == f128(0.0))
            return Detail::quiet_nan<f128>();
        return x - round(x / y) * y;
    }
    [[nodiscard]] inline f256 remainder(const f256 &x, const f256 &y) noexcept {
        if (y == f256(0.0))
            return Detail::quiet_nan<f256>();
        return x - round(x / y) * y;
    }

    template <std::floating_point T>
    [[nodiscard]] inline T hypot(T x, T y) noexcept {
        return std::hypot(x, y);
    }
    [[nodiscard]] inline f128 hypot(f128 x, f128 y) noexcept { return sqrt(x * x + y * y); }
    [[nodiscard]] inline f256 hypot(const f256 &x, const f256 &y) noexcept { return sqrt(x * x + y * y); }

    template <std::floating_point T>
    [[nodiscard]] inline T lerp(T a, T b, T t) noexcept {
        return std::lerp(a, b, t);
    }
    [[nodiscard]] inline f128 lerp(f128 a, f128 b, f128 t) noexcept { return a + (b - a) * t; }
    [[nodiscard]] inline f256 lerp(const f256 &a, const f256 &b, const f256 &t) noexcept { return a + (b - a) * t; }

    template <std::floating_point T>
    [[nodiscard]] inline T fract(T x) noexcept {
        return x - floor(x);
    }
    [[nodiscard]] inline f128 fract(f128 x) noexcept { return x - floor(x); }
    [[nodiscard]] inline f256 fract(const f256 &x) noexcept { return x - floor(x); }

    template <std::floating_point T>
    [[nodiscard]] inline T saturate(T x) noexcept {
        return x < T(0) ? T(0) : (x > T(1) ? T(1) : x);
    }
    [[nodiscard]] inline f128 saturate(f128 x) noexcept { return x < f128(0.0) ? f128(0.0) : (x > f128(1.0) ? f128(1.0) : x); }
    [[nodiscard]] inline f256 saturate(const f256 &x) noexcept { return x < f256(0.0) ? f256(0.0) : (x > f256(1.0) ? f256(1.0) : x); }

} // namespace SFT::Foundation
