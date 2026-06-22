#pragma once

#include "Foundation/Types.hpp"

#include <cmath>
#include <compare>
#include <utility>

// Wide numeric types: 128- and 256-bit variants of the normal scalar kinds.
//
// Naming follows the same rule as Types.hpp — the number is the storage width in bits, and for
// the integers that is also the exact precision. The floats are extended-precision composites
// (a number stored as the unevaluated sum of several f64s, a.k.a. "double-double" / "quad-double"
// arithmetic): f128 keeps ~106 significant bits in 128 bits of storage, f256 keeps ~212 in 256.
//
// IMPORTANT — these are NOT free. There is no CPU instruction that does >64-bit scalar math in one
// op; SIMD registers are lane-partitioned and cannot carry across a single wide value. The cost is
// kept as low as the hardware allows by leaning on what does exist: native __int128 (hardware ADD;
// ADC carry chains) for the 128-bit integers, and FMA-based error-free transforms for the floats.
// A 128-bit add is ~1 extra instruction; multiply/divide and the 256-bit/float types cost more.
// SIMD only helps when a consumer processes *batches* of these (throughput), never a single value.

namespace SFT::Foundation {

#if !defined(__SIZEOF_INT128__)
#error "SturdyEngine's wide integer types require a compiler with __int128 (GCC/Clang)."
#endif

// The f128/f256 algorithms rely on strict IEEE rounding of each individual operation: the
// error-free transforms (two_sum/two_prod) extract the rounding error that -ffast-math assumes
// away, so fast-math silently collapses them to garbage. Fail loudly rather than miscompute.
#if defined(__FAST_MATH__)
#error "SturdyEngine's f128/f256 require IEEE FP semantics; -ffast-math/-Ofast breaks them."
#endif

    // --- 128-bit integers: hardware-backed (native __int128). -------------------------------
    using i128 = __int128;
    using u128 = unsigned __int128;

    // --- 256-bit unsigned integer: two u128 limbs, exact. -----------------------------------
    class u256 {
      public:
        u128 lo{};
        u128 hi{};

        constexpr u256() noexcept = default;
        constexpr u256(u128 low) noexcept : lo(low) {}
        static constexpr u256 from_parts(u128 high, u128 low) noexcept {
            u256 v;
            v.hi = high;
            v.lo = low;
            return v;
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return lo != 0 || hi != 0; }
        [[nodiscard]] constexpr explicit operator u128() const noexcept { return lo; }
        [[nodiscard]] constexpr explicit operator u64() const noexcept { return static_cast<u64>(lo); }
        [[nodiscard]] explicit operator f64() const noexcept {
            return static_cast<f64>(hi) * 0x1p128 + static_cast<f64>(lo); // 2^128 is exact in f64
        }

        friend constexpr std::strong_ordering operator<=>(const u256 &a, const u256 &b) noexcept {
            if (a.hi != b.hi)
                return a.hi < b.hi ? std::strong_ordering::less : std::strong_ordering::greater;
            if (a.lo != b.lo)
                return a.lo < b.lo ? std::strong_ordering::less : std::strong_ordering::greater;
            return std::strong_ordering::equal;
        }
        friend constexpr bool operator==(const u256 &a, const u256 &b) noexcept { return a.hi == b.hi && a.lo == b.lo; }

        constexpr u256 operator~() const noexcept { return from_parts(~hi, ~lo); }
        friend constexpr u256 operator&(u256 a, u256 b) noexcept { return from_parts(a.hi & b.hi, a.lo & b.lo); }
        friend constexpr u256 operator|(u256 a, u256 b) noexcept { return from_parts(a.hi | b.hi, a.lo | b.lo); }
        friend constexpr u256 operator^(u256 a, u256 b) noexcept { return from_parts(a.hi ^ b.hi, a.lo ^ b.lo); }

        friend constexpr u256 operator<<(u256 a, unsigned s) noexcept {
            if (s == 0)
                return a;
            if (s >= 256)
                return u256{};
            if (s >= 128)
                return from_parts(a.lo << (s - 128), 0);
            return from_parts((a.hi << s) | (a.lo >> (128 - s)), a.lo << s);
        }
        friend constexpr u256 operator>>(u256 a, unsigned s) noexcept {
            if (s == 0)
                return a;
            if (s >= 256)
                return u256{};
            if (s >= 128)
                return from_parts(0, a.hi >> (s - 128));
            return from_parts(a.hi >> s, (a.lo >> s) | (a.hi << (128 - s)));
        }

        friend constexpr u256 operator+(u256 a, u256 b) noexcept {
            const u128 lo = a.lo + b.lo;
            const u128 carry = lo < a.lo ? 1 : 0;
            return from_parts(a.hi + b.hi + carry, lo);
        }
        friend constexpr u256 operator-(u256 a, u256 b) noexcept {
            const u128 lo = a.lo - b.lo;
            const u128 borrow = a.lo < b.lo ? 1 : 0;
            return from_parts(a.hi - b.hi - borrow, lo);
        }
        friend constexpr u256 operator*(u256 a, u256 b) noexcept {
            u256 r = full_mul(a.lo, b.lo);
            r.hi = r.hi + a.lo * b.hi + a.hi * b.lo; // higher cross terms overflow out of 256 bits
            return r;
        }
        friend constexpr u256 operator/(u256 a, u256 b) noexcept { return divmod(a, b).first; }
        friend constexpr u256 operator%(u256 a, u256 b) noexcept { return divmod(a, b).second; }

        constexpr u256 operator+() const noexcept { return *this; }
        constexpr u256 &operator+=(u256 o) noexcept { return *this = *this + o; }
        constexpr u256 &operator-=(u256 o) noexcept { return *this = *this - o; }
        constexpr u256 &operator*=(u256 o) noexcept { return *this = *this * o; }
        constexpr u256 &operator/=(u256 o) noexcept { return *this = *this / o; }
        constexpr u256 &operator%=(u256 o) noexcept { return *this = *this % o; }
        constexpr u256 &operator&=(u256 o) noexcept { return *this = *this & o; }
        constexpr u256 &operator|=(u256 o) noexcept { return *this = *this | o; }
        constexpr u256 &operator^=(u256 o) noexcept { return *this = *this ^ o; }
        constexpr u256 &operator<<=(unsigned s) noexcept { return *this = *this << s; }
        constexpr u256 &operator>>=(unsigned s) noexcept { return *this = *this >> s; }
        constexpr u256 &operator++() noexcept { return *this = *this + u256{1}; }
        constexpr u256 &operator--() noexcept { return *this = *this - u256{1}; }
        constexpr u256 operator++(int) noexcept {
            u256 t = *this;
            ++*this;
            return t;
        }
        constexpr u256 operator--(int) noexcept {
            u256 t = *this;
            --*this;
            return t;
        }

        // Position of the highest set bit + 1 (0 for a zero value); drives the division loop bound.
        [[nodiscard]] constexpr unsigned bit_length() const noexcept {
            if (hi != 0)
                return 128u + u128_bit_length(hi);
            return u128_bit_length(lo);
        }

        // Full 128x128 -> 256 product, the one place a true wide multiply is needed.
        static constexpr u256 full_mul(u128 a, u128 b) noexcept {
            const u128 mask = ~static_cast<u128>(0) >> 64;
            const u64 aL = static_cast<u64>(a), aH = static_cast<u64>(a >> 64);
            const u64 bL = static_cast<u64>(b), bH = static_cast<u64>(b >> 64);
            const u128 ll = static_cast<u128>(aL) * bL;
            const u128 lh = static_cast<u128>(aL) * bH;
            const u128 hl = static_cast<u128>(aH) * bL;
            const u128 hh = static_cast<u128>(aH) * bH;
            const u128 mid = (ll >> 64) + (lh & mask) + (hl & mask);
            const u128 low = (ll & mask) | (mid << 64);
            const u128 high = hh + (lh >> 64) + (hl >> 64) + (mid >> 64);
            return from_parts(high, low);
        }

        // Unsigned division/modulo, with fast paths for the common shapes:
        //   * divisor that fits in 64 bits  -> one base-2^64 long-division sweep (4 hardware
        //     128/64 divides) — this is the path decimal formatting and small constants hit;
        //   * dividend < divisor            -> trivial;
        //   * otherwise                     -> shift-subtract, bounded to the dividend's actual
        //     bit length (not a flat 256), so small operands cost proportionally little.
        static constexpr std::pair<u256, u256> divmod(u256 n, u256 d) noexcept {
            if (!static_cast<bool>(d))
                return {u256{}, u256{}}; // division by zero -> {0,0} (caller's contract)
            if (n < d)
                return {u256{}, n};

            if (d.hi == 0 && (d.lo >> 64) == 0) {
                const u64 dv = static_cast<u64>(d.lo);
                const u64 in[4] = {static_cast<u64>(n.lo), static_cast<u64>(n.lo >> 64), static_cast<u64>(n.hi), static_cast<u64>(n.hi >> 64)};
                u64 out[4] = {};
                u128 rem = 0;
                for (int i = 3; i >= 0; --i) {
                    const u128 acc = (rem << 64) | in[i];
                    out[i] = static_cast<u64>(acc / dv);
                    rem = acc % dv;
                }
                const u256 quot = from_parts((static_cast<u128>(out[3]) << 64) | out[2],
                                             (static_cast<u128>(out[1]) << 64) | out[0]);
                return {quot, u256{static_cast<u128>(rem)}};
            }

            const unsigned shift = n.bit_length() - d.bit_length();
            u256 dd = d << shift;
            u256 q{};
            for (unsigned i = 0; i <= shift; ++i) {
                q = q << 1;
                if (n >= dd) {
                    n = n - dd;
                    q.lo |= 1;
                }
                dd = dd >> 1;
            }
            return {q, n};
        }

      private:
        static constexpr unsigned u128_bit_length(u128 v) noexcept {
            const u64 high = static_cast<u64>(v >> 64);
            if (high != 0)
                return 128u - static_cast<unsigned>(__builtin_clzll(high));
            const u64 low = static_cast<u64>(v);
            return low != 0 ? 64u - static_cast<unsigned>(__builtin_clzll(low)) : 0u;
        }
    };

    // --- 256-bit signed integer: two's complement over u256. --------------------------------
    class i256 {
      public:
        u256 bits{};

        constexpr i256() noexcept = default;
        constexpr i256(i128 v) noexcept {
            bits.lo = static_cast<u128>(v);
            bits.hi = v < 0 ? ~static_cast<u128>(0) : 0; // sign-extend
        }
        static constexpr i256 from_bits(u256 b) noexcept {
            i256 v;
            v.bits = b;
            return v;
        }

        [[nodiscard]] constexpr bool is_negative() const noexcept { return (bits.hi >> 127) & 1; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(bits); }
        [[nodiscard]] constexpr explicit operator u256() const noexcept { return bits; }
        [[nodiscard]] constexpr explicit operator i128() const noexcept { return static_cast<i128>(bits.lo); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept { return static_cast<i64>(bits.lo); }
        [[nodiscard]] explicit operator f64() const noexcept {
            const f64 m = static_cast<f64>(magnitude());
            return is_negative() ? -m : m;
        }

        constexpr i256 operator+() const noexcept { return *this; }
        constexpr i256 operator-() const noexcept { return from_bits(~bits + u256{1}); }
        constexpr i256 operator~() const noexcept { return from_bits(~bits); }

        friend constexpr bool operator==(const i256 &a, const i256 &b) noexcept { return a.bits == b.bits; }
        friend constexpr std::strong_ordering operator<=>(const i256 &a, const i256 &b) noexcept {
            if (a.is_negative() != b.is_negative()) {
                return a.is_negative() ? std::strong_ordering::less : std::strong_ordering::greater;
            }
            return a.bits <=> b.bits; // same sign: unsigned bit order agrees with signed order
        }

        // Add/sub/mul are bit-identical to unsigned in two's complement.
        friend constexpr i256 operator+(i256 a, i256 b) noexcept { return from_bits(a.bits + b.bits); }
        friend constexpr i256 operator-(i256 a, i256 b) noexcept { return from_bits(a.bits - b.bits); }
        friend constexpr i256 operator*(i256 a, i256 b) noexcept { return from_bits(a.bits * b.bits); }
        friend constexpr i256 operator/(i256 a, i256 b) noexcept {
            const bool neg = a.is_negative() != b.is_negative();
            const u256 q = u256::divmod(a.magnitude(), b.magnitude()).first;
            return neg ? -from_bits(q) : from_bits(q);
        }
        friend constexpr i256 operator%(i256 a, i256 b) noexcept {
            const u256 r = u256::divmod(a.magnitude(), b.magnitude()).second;
            return a.is_negative() ? -from_bits(r) : from_bits(r); // remainder takes the dividend's sign
        }

        friend constexpr i256 operator&(i256 a, i256 b) noexcept { return from_bits(a.bits & b.bits); }
        friend constexpr i256 operator|(i256 a, i256 b) noexcept { return from_bits(a.bits | b.bits); }
        friend constexpr i256 operator^(i256 a, i256 b) noexcept { return from_bits(a.bits ^ b.bits); }
        friend constexpr i256 operator<<(i256 a, unsigned s) noexcept { return from_bits(a.bits << s); }
        friend constexpr i256 operator>>(i256 a, unsigned s) noexcept { // arithmetic: sign-extends
            if (s == 0)
                return a;
            if (s >= 256)
                return a.is_negative() ? from_bits(~u256{}) : i256{};
            u256 r = a.bits >> s;
            if (a.is_negative())
                r = r | (~u256{} << (256u - s));
            return from_bits(r);
        }

        constexpr i256 &operator+=(i256 o) noexcept { return *this = *this + o; }
        constexpr i256 &operator-=(i256 o) noexcept { return *this = *this - o; }
        constexpr i256 &operator*=(i256 o) noexcept { return *this = *this * o; }
        constexpr i256 &operator/=(i256 o) noexcept { return *this = *this / o; }
        constexpr i256 &operator%=(i256 o) noexcept { return *this = *this % o; }
        constexpr i256 &operator&=(i256 o) noexcept { return *this = *this & o; }
        constexpr i256 &operator|=(i256 o) noexcept { return *this = *this | o; }
        constexpr i256 &operator^=(i256 o) noexcept { return *this = *this ^ o; }
        constexpr i256 &operator<<=(unsigned s) noexcept { return *this = *this << s; }
        constexpr i256 &operator>>=(unsigned s) noexcept { return *this = *this >> s; }
        constexpr i256 &operator++() noexcept { return *this = *this + i256((i128)1); }
        constexpr i256 &operator--() noexcept { return *this = *this - i256((i128)1); }
        constexpr i256 operator++(int) noexcept {
            i256 t = *this;
            ++*this;
            return t;
        }
        constexpr i256 operator--(int) noexcept {
            i256 t = *this;
            --*this;
            return t;
        }

      private:
        [[nodiscard]] constexpr u256 magnitude() const noexcept { return is_negative() ? (-*this).bits : bits; }
    };

    namespace Detail {

        // Error-free transforms — the building blocks of extended-precision float arithmetic.
        struct TwoF64 {
            f64 hi;
            f64 lo;
        };
        inline TwoF64 quick_two_sum(f64 a, f64 b) noexcept { // requires |a| >= |b|
            const f64 s = a + b;
            return {s, b - (s - a)};
        }
        inline TwoF64 two_sum(f64 a, f64 b) noexcept {
            const f64 s = a + b;
            const f64 v = s - a;
            return {s, (a - (s - v)) + (b - v)};
        }
        inline TwoF64 two_prod(f64 a, f64 b) noexcept {
            const f64 p = a * b;
            return {p, std::fma(a, b, -p)};
        }

    } // namespace Detail

    // --- 128-bit float: double-double (~106 significant bits). ------------------------------
    class f128 {
      public:
        f64 hi{};
        f64 lo{};

        constexpr f128() noexcept = default;
        constexpr f128(f64 value) noexcept : hi(value) {}
        constexpr f128(f64 high, f64 low) noexcept : hi(high), lo(low) {}

        [[nodiscard]] constexpr explicit operator f64() const noexcept { return hi + lo; }
        [[nodiscard]] constexpr explicit operator f32() const noexcept { return static_cast<f32>(hi + lo); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept {
            return static_cast<i64>(hi) + static_cast<i64>(lo); // truncates toward zero
        }

        constexpr f128 operator+() const noexcept { return *this; }
        constexpr f128 operator-() const noexcept { return {-hi, -lo}; }

        friend f128 operator+(f128 a, f128 b) noexcept {
            Detail::TwoF64 s = Detail::two_sum(a.hi, b.hi);
            const Detail::TwoF64 e = Detail::two_sum(a.lo, b.lo);
            s.lo += e.hi;
            s = Detail::quick_two_sum(s.hi, s.lo);
            s.lo += e.lo;
            s = Detail::quick_two_sum(s.hi, s.lo);
            return {s.hi, s.lo};
        }
        friend f128 operator-(f128 a, f128 b) noexcept { return a + (-b); }
        friend f128 operator*(f128 a, f128 b) noexcept {
            Detail::TwoF64 p = Detail::two_prod(a.hi, b.hi);
            p.lo += a.hi * b.lo + a.lo * b.hi;
            p = Detail::quick_two_sum(p.hi, p.lo);
            return {p.hi, p.lo};
        }
        friend f128 operator/(f128 a, f128 b) noexcept {
            const f64 q1 = a.hi / b.hi;
            a = a - b * f128(q1);
            const f64 q2 = a.hi / b.hi;
            a = a - b * f128(q2);
            const f64 q3 = a.hi / b.hi;
            Detail::TwoF64 r = Detail::quick_two_sum(q1, q2);
            f128 res{r.hi, r.lo};
            return res + f128(q3);
        }

        f128 &operator+=(f128 o) noexcept { return *this = *this + o; }
        f128 &operator-=(f128 o) noexcept { return *this = *this - o; }
        f128 &operator*=(f128 o) noexcept { return *this = *this * o; }
        f128 &operator/=(f128 o) noexcept { return *this = *this / o; }
        f128 &operator++() noexcept { return *this = *this + f128(1.0); }
        f128 &operator--() noexcept { return *this = *this - f128(1.0); }
        f128 operator++(int) noexcept {
            f128 t = *this;
            ++*this;
            return t;
        }
        f128 operator--(int) noexcept {
            f128 t = *this;
            --*this;
            return t;
        }

        friend constexpr bool operator==(f128 a, f128 b) noexcept { return a.hi == b.hi && a.lo == b.lo; }
        friend constexpr std::partial_ordering operator<=>(f128 a, f128 b) noexcept {
            if (a.hi != b.hi)
                return a.hi <=> b.hi;
            return a.lo <=> b.lo;
        }
    };

    namespace Detail {

        inline f64 qsum(f64 a, f64 b, f64 &err) noexcept {
            const f64 s = a + b;
            err = b - (s - a);
            return s;
        }
        inline f64 tsum(f64 a, f64 b, f64 &err) noexcept {
            const f64 s = a + b;
            const f64 v = s - a;
            err = (a - (s - v)) + (b - v);
            return s;
        }
        inline f64 tprod(f64 a, f64 b, f64 &err) noexcept {
            const f64 p = a * b;
            err = std::fma(a, b, -p);
            return p;
        }
        inline void three_sum(f64 &a, f64 &b, f64 &c) noexcept {
            f64 t1, t2, t3;
            t1 = tsum(a, b, t2);
            a = tsum(c, t1, t3);
            b = tsum(t2, t3, c);
        }
        inline void three_sum2(f64 &a, f64 &b, f64 c) noexcept {
            f64 t1, t2, t3;
            t1 = tsum(a, b, t2);
            a = tsum(c, t1, t3);
            b = t2 + t3;
        }
        // Renormalize 5 overlapping components into 4 non-overlapping ones (Hida-Li-Bailey).
        inline void renorm5(f64 &c0, f64 &c1, f64 &c2, f64 &c3, f64 c4) noexcept {
            f64 s0, s1, s2 = 0.0, s3 = 0.0;
            s0 = qsum(c3, c4, c4);
            s0 = qsum(c2, s0, c3);
            s0 = qsum(c1, s0, c2);
            c0 = qsum(c0, s0, c1);
            s0 = c0;
            s1 = c1;
            s0 = qsum(c0, c1, s1);
            if (s1 != 0.0) {
                s1 = qsum(s1, c2, s2);
                if (s2 != 0.0) {
                    s2 = qsum(s2, c3, s3);
                    if (s3 != 0.0)
                        s3 += c4;
                    else
                        s2 += c4;
                } else {
                    s1 = qsum(s1, c3, s2);
                    if (s2 != 0.0)
                        s2 = qsum(s2, c4, s3);
                    else
                        s1 = qsum(s1, c4, s2);
                }
            } else {
                s0 = qsum(s0, c2, s1);
                if (s1 != 0.0) {
                    s1 = qsum(s1, c3, s2);
                    if (s2 != 0.0)
                        s2 = qsum(s2, c4, s3);
                    else
                        s1 = qsum(s1, c4, s2);
                } else {
                    s0 = qsum(s0, c3, s1);
                    if (s1 != 0.0)
                        s1 = qsum(s1, c4, s2);
                    else
                        s0 = qsum(s0, c4, s1);
                }
            }
            c0 = s0;
            c1 = s1;
            c2 = s2;
            c3 = s3;
        }

    } // namespace Detail

    // --- 256-bit float: quad-double (~212 significant bits). --------------------------------
    class f256 {
      public:
        f64 x[4]{};

        constexpr f256() noexcept = default;
        constexpr f256(f64 value) noexcept : x{value, 0.0, 0.0, 0.0} {}
        constexpr f256(f64 a, f64 b, f64 c, f64 d) noexcept : x{a, b, c, d} {}
        constexpr f256(f128 value) noexcept : x{value.hi, value.lo, 0.0, 0.0} {}

        [[nodiscard]] constexpr explicit operator f64() const noexcept { return x[0]; }
        [[nodiscard]] constexpr explicit operator f32() const noexcept { return static_cast<f32>(x[0]); }
        [[nodiscard]] constexpr explicit operator f128() const noexcept { return f128(x[0], x[1]); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept {
            return static_cast<i64>(x[0]) + static_cast<i64>(x[1]); // truncates toward zero
        }

        constexpr f256 operator+() const noexcept { return *this; }
        constexpr f256 operator-() const noexcept { return {-x[0], -x[1], -x[2], -x[3]}; }

        friend f256 operator+(const f256 &a, const f256 &b) noexcept {
            // Pairwise two_sum of the components, then accumulate and renormalize (sloppy_add).
            f64 t0, t1, t2, t3;
            f64 s0 = Detail::tsum(a.x[0], b.x[0], t0);
            f64 s1 = Detail::tsum(a.x[1], b.x[1], t1);
            f64 s2 = Detail::tsum(a.x[2], b.x[2], t2);
            f64 s3 = Detail::tsum(a.x[3], b.x[3], t3);
            s1 = Detail::tsum(s1, t0, t0);
            Detail::three_sum(s2, t0, t1);
            Detail::three_sum2(s3, t0, t2);
            t0 = t0 + t1 + t3;
            Detail::renorm5(s0, s1, s2, s3, t0);
            return {s0, s1, s2, s3};
        }
        friend f256 operator-(const f256 &a, const f256 &b) noexcept { return a + (-b); }
        friend f256 operator*(const f256 &a, const f256 &b) noexcept {
            // Accumulate the partial products by magnitude tier, then renormalize (sloppy_mul).
            f64 p0, p1, p2, p3, p4, p5;
            f64 q0, q1, q2, q3, q4, q5;
            p0 = Detail::tprod(a.x[0], b.x[0], q0);
            p1 = Detail::tprod(a.x[0], b.x[1], q1);
            p2 = Detail::tprod(a.x[1], b.x[0], q2);
            p3 = Detail::tprod(a.x[0], b.x[2], q3);
            p4 = Detail::tprod(a.x[1], b.x[1], q4);
            p5 = Detail::tprod(a.x[2], b.x[0], q5);
            Detail::three_sum(p1, p2, q0);
            Detail::three_sum(p2, q1, q2);
            Detail::three_sum(p3, p4, p5);
            f64 t0, t1;
            f64 s0 = Detail::tsum(p2, p3, t0);
            f64 s1 = Detail::tsum(q1, p4, t1);
            f64 s2 = q2 + p5;
            s1 = Detail::tsum(s1, t0, t0);
            s2 = s2 + (t0 + t1);
            f64 a0b3 = a.x[0] * b.x[3];
            f64 a1b2 = a.x[1] * b.x[2];
            f64 a2b1 = a.x[2] * b.x[1];
            f64 a3b0 = a.x[3] * b.x[0];
            s2 = s2 + (((q3 + q4) + q5) + ((a0b3 + a1b2) + (a2b1 + a3b0)));
            Detail::renorm5(p0, p1, s0, s1, s2);
            return {p0, p1, s0, s1};
        }
        friend f256 operator/(const f256 &a, const f256 &b) noexcept {
            // Long division: refine the quotient one f64 digit at a time.
            f64 q0 = a.x[0] / b.x[0];
            f256 r = a - b * f256(q0);
            f64 q1 = r.x[0] / b.x[0];
            r = r - b * f256(q1);
            f64 q2 = r.x[0] / b.x[0];
            r = r - b * f256(q2);
            f64 q3 = r.x[0] / b.x[0];
            r = r - b * f256(q3);
            f64 q4 = r.x[0] / b.x[0]; // final digit estimate, folded in by the renormalization
            Detail::renorm5(q0, q1, q2, q3, q4);
            return {q0, q1, q2, q3};
        }

        f256 &operator+=(const f256 &o) noexcept { return *this = *this + o; }
        f256 &operator-=(const f256 &o) noexcept { return *this = *this - o; }
        f256 &operator*=(const f256 &o) noexcept { return *this = *this * o; }
        f256 &operator/=(const f256 &o) noexcept { return *this = *this / o; }
        f256 &operator++() noexcept { return *this = *this + f256(1.0); }
        f256 &operator--() noexcept { return *this = *this - f256(1.0); }
        f256 operator++(int) noexcept {
            f256 t = *this;
            ++*this;
            return t;
        }
        f256 operator--(int) noexcept {
            f256 t = *this;
            --*this;
            return t;
        }

        friend constexpr bool operator==(const f256 &a, const f256 &b) noexcept {
            return a.x[0] == b.x[0] && a.x[1] == b.x[1] && a.x[2] == b.x[2] && a.x[3] == b.x[3];
        }
        friend constexpr std::partial_ordering operator<=>(const f256 &a, const f256 &b) noexcept {
            for (int i = 0; i < 4; ++i) {
                if (a.x[i] != b.x[i])
                    return a.x[i] <=> b.x[i];
            }
            return std::partial_ordering::equivalent;
        }
    };

} // namespace SFT::Foundation

namespace SFT {
    using Foundation::f128;
    using Foundation::f256;
    using Foundation::i128;
    using Foundation::i256;
    using Foundation::u128;
    using Foundation::u256;
} // namespace SFT
