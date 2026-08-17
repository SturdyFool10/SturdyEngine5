#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>


using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::int8_t;
using std::is_standard_layout_v;
using std::is_trivially_copyable_v;
using std::numeric_limits;
using std::ptrdiff_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

namespace SFT::Foundation {


    using i8 = int8_t;
    using i16 = int16_t;
    using i32 = int32_t;
    using i64 = int64_t;

    using u8 = uint8_t;
    using u16 = uint16_t;
    using u32 = uint32_t;
    using u64 = uint64_t;

    using f32 = float;
    using f64 = double;

    using byte = std::byte;
    using usize = size_t;
    using isize = ptrdiff_t;


    class b8 {
      public:
        /// Constructs a `b8` in its default state.
        ///
        /// @note This function does not throw exceptions.
        constexpr b8() noexcept = default;
        /// Constructs a `b8` from the supplied initialization values.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @note This function does not throw exceptions.
        constexpr b8(bool value) noexcept
            : value_(value ? u8{1} : u8{0}) {
        }

        /// Converts the `b8` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return value_ != 0;
        }

        /// Returns the current or globally available value value.
        ///
        /// @return Returns the current value value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr u8 value() const noexcept {
            return value_;
        }

      private:
        u8 value_ = 0;
    };

    static_assert(sizeof(i8) == 1);
    static_assert(sizeof(i16) == 2);
    static_assert(sizeof(i32) == 4);
    static_assert(sizeof(i64) == 8);

    static_assert(sizeof(u8) == 1);
    static_assert(sizeof(u16) == 2);
    static_assert(sizeof(u32) == 4);
    static_assert(sizeof(u64) == 8);

    static_assert(sizeof(f32) == 4);
    static_assert(sizeof(f64) == 8);

    static_assert(sizeof(byte) == 1);
    static_assert(sizeof(b8) == 1);

    static_assert(numeric_limits<f32>::is_iec559, "f32 must be IEEE-754.");
    static_assert(numeric_limits<f64>::is_iec559, "f64 must be IEEE-754.");
    static_assert(is_trivially_copyable_v<b8>);
    static_assert(is_standard_layout_v<b8>);


    /// Performs the assert type assumptions operation using the supplied arguments.
    ///
    /// @pre `sizeof(i8) == 1`; debug builds assert if this precondition is violated.
    /// @pre `sizeof(i16) == 2`; debug builds assert if this precondition is violated.
    /// @pre `sizeof(i32) == 4`; debug builds assert if this precondition is violated.
    /// @note This function does not throw exceptions.
    void assert_type_assumptions() noexcept;

    namespace Detail {

#if !defined(NDEBUG)
        [[maybe_unused]] inline const bool type_assumptions_checked = []() noexcept {
            assert_type_assumptions();
            return true;
        }();
#endif

    } // namespace Detail


} // namespace SFT::Foundation

/// Implements `operator""_i8` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::i8 operator""_i8(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i8>(v); }
/// Implements `operator""_i16` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::i16 operator""_i16(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i16>(v); }
/// Implements `operator""_i32` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::i32 operator""_i32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i32>(v); }
/// Implements `operator""_i64` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::i64 operator""_i64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i64>(v); }
/// Implements `operator""_u8` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::u8 operator""_u8(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u8>(v); }
/// Implements `operator""_u16` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::u16 operator""_u16(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u16>(v); }
/// Implements `operator""_u32` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::u32 operator""_u32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u32>(v); }
/// Implements `operator""_u64` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::u64 operator""_u64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u64>(v); }
/// Implements `operator""_usize` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::usize operator""_usize(unsigned long long v) noexcept { return static_cast<SFT::Foundation::usize>(v); }
/// Implements `operator""_isize` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::isize operator""_isize(unsigned long long v) noexcept { return static_cast<SFT::Foundation::isize>(v); }
/// Implements `operator""_f32` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::f32 operator""_f32(long double v) noexcept { return static_cast<SFT::Foundation::f32>(v); }
/// Implements `operator""_f32` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::f32 operator""_f32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::f32>(v); }
/// Implements `operator""_f64` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::f64 operator""_f64(long double v) noexcept { return static_cast<SFT::Foundation::f64>(v); }
/// Implements `operator""_f64` for `/mnt/data/SE_docpass/SturdyEngine5-Header-Cleaned/Foundation/src/Types.hpp`.
///
/// @param v `v` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] constexpr SFT::Foundation::f64 operator""_f64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::f64>(v); }

namespace SFT::Foundation {

    namespace Literals {
        using ::operator""_f32;
        using ::operator""_f64;
        using ::operator""_i8;
        using ::operator""_i16;
        using ::operator""_i32;
        using ::operator""_i64;
        using ::operator""_isize;
        using ::operator""_u8;
        using ::operator""_u16;
        using ::operator""_u32;
        using ::operator""_u64;
        using ::operator""_usize;
    } // namespace Literals

    namespace Detail {
        /// Returns the current or globally available scalar literal smoke test value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] consteval bool scalar_literal_smoke_test() noexcept {
            using namespace SFT::Foundation::Literals;
            return 200_u8 == static_cast<u8>(200) && 5_i32 == 5 && 0xFF_u32 == 255u && 1'000_u64 == 1000u &&
                   16_usize == usize{16} && static_cast<f64>(2.5_f64) == 2.5 && static_cast<f32>(5_f32) == 5.0f;
        }
        static_assert(scalar_literal_smoke_test());
    } // namespace Detail

} // namespace SFT::Foundation

// The scalar types are re-exported unqualified into `SFT` so engine code can write `u32`, `f64`, `b8`
// directly without the `Foundation::` prefix. Everything else in Foundation stays namespace-qualified.
namespace SFT {

    using b8 = Foundation::b8;
    using Foundation::byte;
    using Foundation::f32;
    using Foundation::f64;
    using Foundation::i16;
    using Foundation::i32;
    using Foundation::i64;
    using Foundation::i8;
    using Foundation::isize;
    using Foundation::u16;
    using Foundation::u32;
    using Foundation::u64;
    using Foundation::u8;
    using Foundation::usize;

    static_assert(sizeof(b8) == 1);

} // namespace SFT
