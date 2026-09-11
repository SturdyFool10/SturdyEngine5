#pragma once

#include <Foundation/Types.hpp>

#include <string_view>


namespace SFT::Foundation {


    /// A 128-bit dual-stream FNV-1a hash, used anywhere a stable, low-collision
    /// name-derived key is needed (component/type registries, etc).
    struct Fnv1a128 {
        u64 high = 0;
        u64 low = 0;

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(Fnv1a128, Fnv1a128) noexcept = default;

        /// Hashes the supplied bytes into a 128-bit FNV-1a value.
        ///
        /// @param bytes Byte span consumed by the operation.
        ///
        /// @return Returns the newly constructed hash value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr Fnv1a128 from_bytes(std::string_view bytes) noexcept {
            constexpr u64 fnv_prime = 1099511628211ull;
            u64 high_hash = 14695981039346656037ull;
            u64 low_hash = 7809847782465536322ull;
            for (char character : bytes) {
                const auto byte = static_cast<u8>(static_cast<unsigned char>(character));
                high_hash = (high_hash ^ byte) * fnv_prime;
                low_hash = (low_hash ^ static_cast<u8>(byte + 0x9du)) * (fnv_prime + 2ull);
            }
            return Fnv1a128{.high = high_hash, .low = low_hash};
        }
    };

    static_assert(sizeof(Fnv1a128) == sizeof(u64) * 2);
    static_assert(std::is_standard_layout_v<Fnv1a128>);
    static_assert(std::is_trivially_copyable_v<Fnv1a128>);

    /// Mixes a `Fnv1a128` value down to a single `usize`, for use as an
    /// `std::unordered_map`/`std::unordered_set` hash functor.
    struct Fnv1a128Hash {
        /// Invokes the callable behavior provided by `Fnv1a128Hash`.
        ///
        /// @param value Value used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr usize operator()(Fnv1a128 value) const noexcept {
            const u64 mixed = value.low ^ (value.high + 0x9e3779b97f4a7c15ull + (value.low << 6u) + (value.low >> 2u));
            return static_cast<usize>(mixed);
        }
    };


} // namespace SFT::Foundation
