#include <Foundation/WideTraits.hpp>


namespace SFT::Foundation {

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(u128 v) {
        if (v == 0)
            return "0";

        string digits;
        while (v != 0) {
            digits.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
            v /= 10;
        }
        return string(digits.rbegin(), digits.rend());
    }

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(i128 v) {
        const u128 mag = v < 0 ? (~static_cast<u128>(v) + 1) : static_cast<u128>(v);
        return v < 0 ? "-" + to_string(mag) : to_string(mag);
    }

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(u256 v) {
        if (!static_cast<bool>(v))
            return "0";
        string s;
        while (static_cast<bool>(v)) {
            const auto [q, r] = u256::divmod(v, u256{10});
            s.push_back(static_cast<char>('0' + static_cast<int>(static_cast<u64>(r))));
            v = q;
        }
        return string(s.rbegin(), s.rend());
    }

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(i256 v) {
        return v.is_negative() ? "-" + to_string((-v).bits) : to_string(v.bits);
    }

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(f128 v) { return Detail::wide_float_to_string(v, 31); }

    /// Converts the value to string representation.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value converted to string representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string to_string(const f256 &v) { return Detail::wide_float_to_string(v, 62); }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const u256 &v) { return os << to_string(v); }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const i256 &v) { return os << to_string(v); }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, f128 v) { return os << to_string(v); }

    /// Writes or shifts the left-hand operand using the right-hand value.
    ///
    /// @param os `os` value used by the operation.
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    ostream &operator<<(ostream &os, const f256 &v) { return os << to_string(v); }

} // namespace SFT::Foundation

/// Writes or shifts the left-hand operand using the right-hand value.
///
/// @param os `os` value used by the operation.
/// @param v `v` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ostream &operator<<(ostream &os, __int128 v) { return os << SFT::Foundation::to_string(v); }

/// Writes or shifts the left-hand operand using the right-hand value.
///
/// @param os `os` value used by the operation.
/// @param v `v` value used by the operation.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ostream &operator<<(ostream &os, unsigned __int128 v) { return os << SFT::Foundation::to_string(v); }

namespace SFT::Foundation::Detail {

    /// Hashes mix using the supplied arguments and current state.
    ///
    /// @param seed `seed` value used by the operation.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize hash_mix(usize seed, usize value) noexcept {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }

    /// Hashes u128 using the supplied arguments and current state.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    usize hash_u128(u128 v) noexcept {
        const hash<u64> h;
        return hash_mix(h(static_cast<u64>(v)), h(static_cast<u64>(v >> 64)));
    }

} // namespace SFT::Foundation::Detail

namespace std {

    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    SFT::usize hash<SFT::Foundation::u256>::operator()(const SFT::Foundation::u256 &v) const noexcept {
        return SFT::Foundation::Detail::hash_mix(SFT::Foundation::Detail::hash_u128(v.lo),
                                                 SFT::Foundation::Detail::hash_u128(v.hi));
    }


    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    SFT::usize hash<SFT::Foundation::i256>::operator()(const SFT::Foundation::i256 &v) const noexcept {
        return hash<SFT::Foundation::u256>{}(v.bits);
    }

    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    SFT::usize hash<SFT::Foundation::f128>::operator()(const SFT::Foundation::f128 &v) const noexcept {
        const hash<SFT::f64> h;
        return SFT::Foundation::Detail::hash_mix(h(v.hi == 0.0 ? 0.0 : v.hi), h(v.lo == 0.0 ? 0.0 : v.lo));
    }

    /// Invokes the callable behavior provided by `std`.
    ///
    /// @param v `v` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    SFT::usize hash<SFT::Foundation::f256>::operator()(const SFT::Foundation::f256 &v) const noexcept {
        const hash<SFT::f64> h;
        SFT::usize seed = h(v.x[0] == 0.0 ? 0.0 : v.x[0]);
        for (int i = 1; i < 4; ++i)
            seed = SFT::Foundation::Detail::hash_mix(seed, h(v.x[i] == 0.0 ? 0.0 : v.x[i]));
        return seed;
    }

} // namespace std

