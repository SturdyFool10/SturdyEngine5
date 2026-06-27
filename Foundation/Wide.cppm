module;

#include <cmath>
#include <compare>
#include <concepts>
#include <type_traits>
#include <utility>

export module Sturdy.Foundation:Wide;

import :Types;

using std::fma;
using std::integral;
using std::is_constant_evaluated;
using std::is_signed_v;
using std::pair;
using std::partial_ordering;
using std::same_as;
using std::strong_ordering;

export namespace SFT::Foundation {

#if !defined(__SIZEOF_INT128__)
#error "SturdyEngine's wide integer types require a compiler with __int128 (GCC/Clang)."
#endif

#if defined(__FAST_MATH__)
#error "SturdyEngine's f128/f256 require IEEE FP semantics; -ffast-math/-Ofast breaks them."
#endif

    using i128 = __int128;
    using u128 = unsigned __int128;

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
        [[nodiscard]] constexpr explicit operator f64() const noexcept {
            return static_cast<f64>(hi) * 0x1p128 + static_cast<f64>(lo);
        }

        friend constexpr strong_ordering operator<=>(const u256 &a, const u256 &b) noexcept {
            if (a.hi != b.hi)
                return a.hi < b.hi ? strong_ordering::less : strong_ordering::greater;
            if (a.lo != b.lo)
                return a.lo < b.lo ? strong_ordering::less : strong_ordering::greater;
            return strong_ordering::equal;
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
            r.hi = r.hi + a.lo * b.hi + a.hi * b.lo;
            return r;
        }
        friend constexpr u256 operator/(u256 a, u256 b) noexcept { return divmod(a, b).first; }
        friend constexpr u256 operator%(u256 a, u256 b) noexcept { return divmod(a, b).second; }

        constexpr u256 operator+() const noexcept { return *this; }
        constexpr u256 operator-() const noexcept { return ~*this + u256{1}; }
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

        [[nodiscard]] constexpr unsigned bit_length() const noexcept {
            if (hi != 0)
                return 128u + u128_bit_length(hi);
            return u128_bit_length(lo);
        }

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

        static constexpr pair<u256, u256> divmod(u256 n, u256 d) noexcept {
            if (!static_cast<bool>(d))
                return {u256{}, u256{}};
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

    class i256 {
      public:
        u256 bits{};

        constexpr i256() noexcept = default;
        constexpr i256(i128 v) noexcept {
            bits.lo = static_cast<u128>(v);
            bits.hi = v < 0 ? ~static_cast<u128>(0) : 0;
        }
        static constexpr i256 from_bits(u256 b) noexcept {
            i256 v;
            v.bits = b;
            return v;
        }

        [[nodiscard]] constexpr bool is_negative() const noexcept { return (bits.hi >> 127) & 1; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(bits); }
        [[nodiscard]] constexpr operator u256() const noexcept { return bits; }
        [[nodiscard]] constexpr explicit operator i128() const noexcept { return static_cast<i128>(bits.lo); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept { return static_cast<i64>(bits.lo); }
        [[nodiscard]] constexpr explicit operator f64() const noexcept {
            const f64 m = static_cast<f64>(magnitude());
            return is_negative() ? -m : m;
        }

        constexpr i256 operator+() const noexcept { return *this; }
        constexpr i256 operator-() const noexcept { return from_bits(~bits + u256{1}); }
        constexpr i256 operator~() const noexcept { return from_bits(~bits); }

        friend constexpr bool operator==(const i256 &a, const i256 &b) noexcept { return a.bits == b.bits; }
        friend constexpr strong_ordering operator<=>(const i256 &a, const i256 &b) noexcept {
            if (a.is_negative() != b.is_negative()) {
                return a.is_negative() ? strong_ordering::less : strong_ordering::greater;
            }
            return a.bits <=> b.bits;
        }

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
            return a.is_negative() ? -from_bits(r) : from_bits(r);
        }

        friend constexpr i256 operator&(i256 a, i256 b) noexcept { return from_bits(a.bits & b.bits); }
        friend constexpr i256 operator|(i256 a, i256 b) noexcept { return from_bits(a.bits | b.bits); }
        friend constexpr i256 operator^(i256 a, i256 b) noexcept { return from_bits(a.bits ^ b.bits); }
        friend constexpr i256 operator<<(i256 a, unsigned s) noexcept { return from_bits(a.bits << s); }
        friend constexpr i256 operator>>(i256 a, unsigned s) noexcept {
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

        template <class T>
        concept WideFloatConvertibleInteger =
            integral<T> || same_as<T, i128> || same_as<T, u128> || same_as<T, i256> || same_as<T, u256>;

        template <class F>
        [[nodiscard]] constexpr F u128_to_expansion_float(u128 v) noexcept {
            const F radix(0x1p32);
            F acc(static_cast<f64>(static_cast<u32>(v >> 96)));
            acc = acc * radix + F(static_cast<f64>(static_cast<u32>(v >> 64)));
            acc = acc * radix + F(static_cast<f64>(static_cast<u32>(v >> 32)));
            acc = acc * radix + F(static_cast<f64>(static_cast<u32>(v)));
            return acc;
        }
        template <class F>
        [[nodiscard]] constexpr F u256_to_expansion_float(const u256 &v) noexcept {
            return u128_to_expansion_float<F>(v.hi) * F(0x1p128) + u128_to_expansion_float<F>(v.lo);
        }
        template <class F, class T>
        [[nodiscard]] constexpr F integer_to_expansion_float(const T &value) noexcept {
            if constexpr (same_as<T, u256>) {
                return u256_to_expansion_float<F>(value);
            } else if constexpr (same_as<T, i256>) {
                const F magnitude = u256_to_expansion_float<F>(static_cast<u256>(value.is_negative() ? -value : value));
                return value.is_negative() ? -magnitude : magnitude;
            } else if constexpr (is_signed_v<T> || same_as<T, i128>) {
                const bool negative = value < T(0);
                const u128 bits = negative ? (static_cast<u128>(0) - static_cast<u128>(value)) : static_cast<u128>(value);
                const F magnitude = u128_to_expansion_float<F>(bits);
                return negative ? -magnitude : magnitude;
            } else {
                return u128_to_expansion_float<F>(static_cast<u128>(value));
            }
        }

        struct TwoF64 {
            f64 hi;
            f64 lo;
        };
        [[nodiscard]] constexpr TwoF64 quick_two_sum(f64 a, f64 b) noexcept {
            const f64 s = a + b;
            return {s, b - (s - a)};
        }
        [[nodiscard]] constexpr TwoF64 two_sum(f64 a, f64 b) noexcept {
            const f64 s = a + b;
            const f64 v = s - a;
            return {s, (a - (s - v)) + (b - v)};
        }
        [[nodiscard]] constexpr TwoF64 split_product_operand(f64 a) noexcept {
            constexpr f64 splitter = 134217729.0;
            const f64 c = splitter * a;
            const f64 hi = c - (c - a);
            return {hi, a - hi};
        }
        [[nodiscard]] constexpr TwoF64 two_prod_constexpr(f64 a, f64 b, f64 p) noexcept {
            const TwoF64 as = split_product_operand(a);
            const TwoF64 bs = split_product_operand(b);
            const f64 err = ((as.hi * bs.hi - p) + as.hi * bs.lo + as.lo * bs.hi) + as.lo * bs.lo;
            return {p, err};
        }
        [[nodiscard]] constexpr TwoF64 two_prod(f64 a, f64 b) noexcept {
            const f64 p = a * b;
            if (is_constant_evaluated())
                return two_prod_constexpr(a, b, p);
            return {p, fma(a, b, -p)};
        }

    } // namespace Detail

    class f128 {
      public:
        f64 hi{};
        f64 lo{};

        constexpr f128() noexcept = default;
        constexpr f128(f64 value) noexcept : hi(value) {}
        constexpr f128(f64 high, f64 low) noexcept : hi(high), lo(low) {}
        template <Detail::WideFloatConvertibleInteger T>
        constexpr f128(const T &value) noexcept : f128(Detail::integer_to_expansion_float<f128>(value)) {}

        [[nodiscard]] constexpr explicit operator f64() const noexcept { return hi + lo; }
        [[nodiscard]] constexpr explicit operator f32() const noexcept { return static_cast<f32>(hi + lo); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept {
            return static_cast<i64>(hi) + static_cast<i64>(lo);
        }

        constexpr f128 operator+() const noexcept { return *this; }
        constexpr f128 operator-() const noexcept { return {-hi, -lo}; }

        friend constexpr f128 operator+(f128 a, f128 b) noexcept {
            Detail::TwoF64 s = Detail::two_sum(a.hi, b.hi);
            const Detail::TwoF64 e = Detail::two_sum(a.lo, b.lo);
            s.lo += e.hi;
            s = Detail::quick_two_sum(s.hi, s.lo);
            s.lo += e.lo;
            s = Detail::quick_two_sum(s.hi, s.lo);
            return {s.hi, s.lo};
        }
        friend constexpr f128 operator-(f128 a, f128 b) noexcept { return a + (-b); }
        friend constexpr f128 operator*(f128 a, f128 b) noexcept {
            Detail::TwoF64 p = Detail::two_prod(a.hi, b.hi);
            p.lo += a.hi * b.lo + a.lo * b.hi;
            p = Detail::quick_two_sum(p.hi, p.lo);
            return {p.hi, p.lo};
        }
        friend constexpr f128 operator/(f128 a, f128 b) noexcept {
            const f64 q1 = a.hi / b.hi;
            a = a - b * f128(q1);
            const f64 q2 = a.hi / b.hi;
            a = a - b * f128(q2);
            const f64 q3 = a.hi / b.hi;
            Detail::TwoF64 r = Detail::quick_two_sum(q1, q2);
            f128 res{r.hi, r.lo};
            return res + f128(q3);
        }

        constexpr f128 &operator+=(f128 o) noexcept { return *this = *this + o; }
        constexpr f128 &operator-=(f128 o) noexcept { return *this = *this - o; }
        constexpr f128 &operator*=(f128 o) noexcept { return *this = *this * o; }
        constexpr f128 &operator/=(f128 o) noexcept { return *this = *this / o; }
        constexpr f128 &operator++() noexcept { return *this = *this + f128(1.0); }
        constexpr f128 &operator--() noexcept { return *this = *this - f128(1.0); }
        constexpr f128 operator++(int) noexcept {
            f128 t = *this;
            ++*this;
            return t;
        }
        constexpr f128 operator--(int) noexcept {
            f128 t = *this;
            --*this;
            return t;
        }

        friend constexpr bool operator==(f128 a, f128 b) noexcept { return a.hi == b.hi && a.lo == b.lo; }
        friend constexpr partial_ordering operator<=>(f128 a, f128 b) noexcept {
            if (a.hi != b.hi)
                return a.hi <=> b.hi;
            return a.lo <=> b.lo;
        }
    };

    namespace Detail {

        constexpr f64 qsum(f64 a, f64 b, f64 &err) noexcept {
            const f64 s = a + b;
            err = b - (s - a);
            return s;
        }
        constexpr f64 tsum(f64 a, f64 b, f64 &err) noexcept {
            const f64 s = a + b;
            const f64 v = s - a;
            err = (a - (s - v)) + (b - v);
            return s;
        }
        constexpr f64 tprod(f64 a, f64 b, f64 &err) noexcept {
            const TwoF64 p = two_prod(a, b);
            err = p.lo;
            return p.hi;
        }
        constexpr void three_sum(f64 &a, f64 &b, f64 &c) noexcept {
            f64 t1, t2, t3;
            t1 = tsum(a, b, t2);
            a = tsum(c, t1, t3);
            b = tsum(t2, t3, c);
        }
        constexpr void three_sum2(f64 &a, f64 &b, f64 c) noexcept {
            f64 t1, t2, t3;
            t1 = tsum(a, b, t2);
            a = tsum(c, t1, t3);
            b = t2 + t3;
        }
        constexpr void renorm5(f64 &c0, f64 &c1, f64 &c2, f64 &c3, f64 c4) noexcept {
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

    class f256 {
      public:
        f64 x[4]{};

        constexpr f256() noexcept = default;
        constexpr f256(f64 value) noexcept : x{value, 0.0, 0.0, 0.0} {}
        constexpr f256(f64 a, f64 b, f64 c, f64 d) noexcept : x{a, b, c, d} {}
        constexpr f256(f128 value) noexcept : x{value.hi, value.lo, 0.0, 0.0} {}
        constexpr f256(const f256 &) noexcept = default;
        template <Detail::WideFloatConvertibleInteger T>
        constexpr f256(const T &value) noexcept : f256(Detail::integer_to_expansion_float<f256>(value)) {}

        constexpr f256 &operator=(const f256 &other) noexcept {
            x[0] = other.x[0];
            x[1] = other.x[1];
            x[2] = other.x[2];
            x[3] = other.x[3];
            return *this;
        }

        constexpr f256 &operator=(f256 &&other) noexcept {
            x[0] = other.x[0];
            x[1] = other.x[1];
            x[2] = other.x[2];
            x[3] = other.x[3];
            return *this;
        }

        [[nodiscard]] constexpr explicit operator f64() const noexcept { return x[0]; }
        [[nodiscard]] constexpr explicit operator f32() const noexcept { return static_cast<f32>(x[0]); }
        [[nodiscard]] constexpr explicit operator f128() const noexcept { return f128(x[0], x[1]); }
        [[nodiscard]] constexpr explicit operator i64() const noexcept {
            return static_cast<i64>(x[0]) + static_cast<i64>(x[1]);
        }

        constexpr f256 operator+() const noexcept { return *this; }
        constexpr f256 operator-() const noexcept { return {-x[0], -x[1], -x[2], -x[3]}; }

        friend constexpr f256 operator+(const f256 &a, const f256 &b) noexcept {
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
        friend constexpr f256 operator-(const f256 &a, const f256 &b) noexcept { return a + (-b); }
        friend constexpr f256 operator*(const f256 &a, const f256 &b) noexcept {
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
        friend constexpr f256 operator/(const f256 &a, const f256 &b) noexcept {
            f64 q0 = a.x[0] / b.x[0];
            f256 r = a - b * f256(q0);
            f64 q1 = r.x[0] / b.x[0];
            r = r - b * f256(q1);
            f64 q2 = r.x[0] / b.x[0];
            r = r - b * f256(q2);
            f64 q3 = r.x[0] / b.x[0];
            r = r - b * f256(q3);
            f64 q4 = r.x[0] / b.x[0];
            Detail::renorm5(q0, q1, q2, q3, q4);
            return {q0, q1, q2, q3};
        }

        constexpr f256 &operator+=(const f256 &o) noexcept { return *this = *this + o; }
        constexpr f256 &operator-=(const f256 &o) noexcept { return *this = *this - o; }
        constexpr f256 &operator*=(const f256 &o) noexcept { return *this = *this * o; }
        constexpr f256 &operator/=(const f256 &o) noexcept { return *this = *this / o; }
        constexpr f256 &operator++() noexcept { return *this = *this + f256(1.0); }
        constexpr f256 &operator--() noexcept { return *this = *this - f256(1.0); }
        constexpr f256 operator++(int) noexcept {
            f256 t = *this;
            ++*this;
            return t;
        }
        constexpr f256 operator--(int) noexcept {
            f256 t = *this;
            --*this;
            return t;
        }

        friend constexpr bool operator==(const f256 &a, const f256 &b) noexcept {
            return a.x[0] == b.x[0] && a.x[1] == b.x[1] && a.x[2] == b.x[2] && a.x[3] == b.x[3];
        }
        friend constexpr partial_ordering operator<=>(const f256 &a, const f256 &b) noexcept {
            for (int i = 0; i < 4; ++i) {
                if (a.x[i] != b.x[i])
                    return a.x[i] <=> b.x[i];
            }
            return partial_ordering::equivalent;
        }
    };

    namespace Detail {

        [[nodiscard]] consteval bool wide_constexpr_smoke_test() noexcept {
            constexpr u256 shifted = u256::from_parts(1, 0) >> 128;
            if (static_cast<u64>(shifted) != 1)
                return false;

            constexpr u256 quotient = u256{100} / u256{4};
            if (static_cast<u64>(quotient) != 25)
                return false;

            constexpr f64 wide_as_f64 = static_cast<f64>(u256::from_parts(1, 0));
            if (wide_as_f64 != 0x1p128)
                return false;

            constexpr i256 signed_product = i256((i128)-7) * i256((i128)3);
            if (static_cast<i64>(signed_product) != -21)
                return false;

            constexpr f64 signed_as_f64 = static_cast<f64>(i256((i128)-42));
            if (signed_as_f64 != -42.0)
                return false;

            constexpr f128 double_double = (f128(1.5) + f128(2.25)) * f128(2.0) / f128(3.0);
            if (static_cast<f64>(double_double) != 2.5)
                return false;

            constexpr f256 quad_double = (f256(1.5) + f256(2.25)) * f256(2.0) / f256(3.0);
            return static_cast<f64>(quad_double) == 2.5;
        }

        static_assert(wide_constexpr_smoke_test());

        [[nodiscard]] consteval bool wide_conversion_smoke_test() noexcept {
            if (static_cast<f64>(f256(42)) != 42.0 || static_cast<f64>(f128(-7)) != -7.0)
                return false;
            if (static_cast<f64>(f256(i256((i128)-123456789))) != -123456789.0)
                return false;

            const u128 two_pow_100 = static_cast<u128>(1) << 100;
            if (static_cast<f64>(f256(two_pow_100)) != 0x1p100)
                return false;
            if (!(f256(two_pow_100 + 1) - f256(two_pow_100) == f256(1.0)))
                return false;

            if (static_cast<f64>(f256(1000) - i256((i128)1)) != 999.0)
                return false;

            const u256 u = i256((i128)5);
            return static_cast<u64>(u) == 5u;
        }

        static_assert(wide_conversion_smoke_test());

        template <class U>
        [[nodiscard]] constexpr U parse_wide_unsigned(const char *text) noexcept {
            U base = U(10);
            if (text[0] == '0') {
                if (text[1] == 'x' || text[1] == 'X') {
                    base = U(16);
                    text += 2;
                } else if (text[1] == 'b' || text[1] == 'B') {
                    base = U(2);
                    text += 2;
                } else if (text[1] != '\0') {
                    base = U(8);
                    text += 1;
                }
            }
            U value = U(0);
            for (; *text != '\0'; ++text) {
                const char c = *text;
                if (c == '\'')
                    continue;
                unsigned digit = 0;
                if (c >= '0' && c <= '9')
                    digit = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    digit = static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    digit = static_cast<unsigned>(c - 'A' + 10);
                else
                    break;
                value = value * base + U(digit);
            }
            return value;
        }

        template <class F>
        [[nodiscard]] constexpr F scale_pow10(F value, int exponent) noexcept {
            if (exponent == 0)
                return value;
            int n = exponent < 0 ? -exponent : exponent;
            F factor(1.0);
            F base(10.0);
            while (n != 0) {
                if ((n & 1) != 0)
                    factor = factor * base;
                base = base * base;
                n >>= 1;
            }
            return exponent < 0 ? value / factor : value * factor;
        }

        template <class F>
        [[nodiscard]] constexpr F parse_wide_float(const char *text) noexcept {
            F value(0.0);
            int fraction_digits = 0;
            bool seen_dot = false;
            for (; *text != '\0'; ++text) {
                const char c = *text;
                if (c == '\'')
                    continue;
                if (c == '.') {
                    seen_dot = true;
                    continue;
                }
                if (c < '0' || c > '9')
                    break;
                value = value * F(10.0) + F(static_cast<f64>(c - '0'));
                if (seen_dot)
                    ++fraction_digits;
            }
            int exponent = 0;
            if (*text == 'e' || *text == 'E') {
                ++text;
                bool negative = false;
                if (*text == '+')
                    ++text;
                else if (*text == '-') {
                    negative = true;
                    ++text;
                }
                int magnitude = 0;
                for (; *text >= '0' && *text <= '9'; ++text)
                    magnitude = magnitude * 10 + (*text - '0');
                exponent = negative ? -magnitude : magnitude;
            }
            return scale_pow10<F>(value, exponent - fraction_digits);
        }

    } // namespace Detail

    namespace Literals {

        [[nodiscard]] constexpr u128 operator""_u128(const char *text) noexcept { return Detail::parse_wide_unsigned<u128>(text); }
        [[nodiscard]] constexpr i128 operator""_i128(const char *text) noexcept { return static_cast<i128>(Detail::parse_wide_unsigned<u128>(text)); }
        [[nodiscard]] constexpr u256 operator""_u256(const char *text) noexcept { return Detail::parse_wide_unsigned<u256>(text); }
        [[nodiscard]] constexpr i256 operator""_i256(const char *text) noexcept { return i256::from_bits(Detail::parse_wide_unsigned<u256>(text)); }
        [[nodiscard]] constexpr f128 operator""_f128(const char *text) noexcept { return Detail::parse_wide_float<f128>(text); }
        [[nodiscard]] constexpr f256 operator""_f256(const char *text) noexcept { return Detail::parse_wide_float<f256>(text); }

    } // namespace Literals

    namespace Detail {

        [[nodiscard]] consteval bool wide_literal_smoke_test() noexcept {
            using namespace SFT::Foundation::Literals;
            if (static_cast<u64>(255_u128) != 255u || static_cast<u64>(0xFF_u128) != 255u)
                return false;
            if (static_cast<u64>(1'000_u128) != 1000u)
                return false;
            if (static_cast<u64>(0b1010_u256) != 10u || static_cast<u64>(010_u256) != 8u)
                return false;
            if (static_cast<i64>(-7_i128) != -7)
                return false;
            if (static_cast<bool>(1_u256 + -(1_u256)))
                return false;
            if (static_cast<f64>(2.5_f128) != 2.5 || static_cast<f64>(1.0_f256 + 0.5_f256) != 1.5)
                return false;
            if (static_cast<f64>(1.5e2_f128) != 150.0 || static_cast<f64>(2500e-2_f256) != 25.0)
                return false;
            return true;
        }

        static_assert(wide_literal_smoke_test());

    } // namespace Detail

} // namespace SFT::Foundation

export namespace SFT {
    using Foundation::f128;
    using Foundation::f256;
    using Foundation::i128;
    using Foundation::i256;
    using Foundation::u128;
    using Foundation::u256;
} // namespace SFT
