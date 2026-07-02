module;

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

export module Sturdy.Foundation:Types;

using std::byte;
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

export namespace SFT::Foundation {

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

    using byte = ::byte;
    using usize = size_t;
    using isize = ptrdiff_t;

    class b8 {
      public:
        constexpr b8() noexcept = default;
        constexpr b8(bool value) noexcept
            : value_(value ? u8{1} : u8{0}) {
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return value_ != 0;
        }

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

    inline void assert_type_assumptions() noexcept {
        assert(sizeof(i8) == 1);
        assert(sizeof(i16) == 2);
        assert(sizeof(i32) == 4);
        assert(sizeof(i64) == 8);

        assert(sizeof(u8) == 1);
        assert(sizeof(u16) == 2);
        assert(sizeof(u32) == 4);
        assert(sizeof(u64) == 8);

        assert(sizeof(f32) == 4);
        assert(sizeof(f64) == 8);

        assert(sizeof(byte) == 1);
        assert(sizeof(b8) == 1);

        assert(numeric_limits<f32>::is_iec559);
        assert(numeric_limits<f64>::is_iec559);
        assert(is_trivially_copyable_v<b8>);
        assert(is_standard_layout_v<b8>);
    }

    namespace Detail {

#if !defined(NDEBUG)
        [[maybe_unused]] inline const bool type_assumptions_checked = []() noexcept {
            assert_type_assumptions();
            return true;
        }();
#endif

    } // namespace Detail

    namespace Literals {
        [[nodiscard]] constexpr i8 operator""_i8(unsigned long long v) noexcept { return static_cast<i8>(v); }
        [[nodiscard]] constexpr i16 operator""_i16(unsigned long long v) noexcept { return static_cast<i16>(v); }
        [[nodiscard]] constexpr i32 operator""_i32(unsigned long long v) noexcept { return static_cast<i32>(v); }
        [[nodiscard]] constexpr i64 operator""_i64(unsigned long long v) noexcept { return static_cast<i64>(v); }
        [[nodiscard]] constexpr u8 operator""_u8(unsigned long long v) noexcept { return static_cast<u8>(v); }
        [[nodiscard]] constexpr u16 operator""_u16(unsigned long long v) noexcept { return static_cast<u16>(v); }
        [[nodiscard]] constexpr u32 operator""_u32(unsigned long long v) noexcept { return static_cast<u32>(v); }
        [[nodiscard]] constexpr u64 operator""_u64(unsigned long long v) noexcept { return static_cast<u64>(v); }
        [[nodiscard]] constexpr usize operator""_usize(unsigned long long v) noexcept { return static_cast<usize>(v); }
        [[nodiscard]] constexpr isize operator""_isize(unsigned long long v) noexcept { return static_cast<isize>(v); }
        [[nodiscard]] constexpr f32 operator""_f32(long double v) noexcept { return static_cast<f32>(v); }
        [[nodiscard]] constexpr f32 operator""_f32(unsigned long long v) noexcept { return static_cast<f32>(v); }
        [[nodiscard]] constexpr f64 operator""_f64(long double v) noexcept { return static_cast<f64>(v); }
        [[nodiscard]] constexpr f64 operator""_f64(unsigned long long v) noexcept { return static_cast<f64>(v); }
    } // namespace Literals

    namespace Detail {
        [[nodiscard]] consteval bool scalar_literal_smoke_test() noexcept {
            using namespace SFT::Foundation::Literals;
            return 200_u8 == static_cast<u8>(200) && 5_i32 == 5 && 0xFF_u32 == 255u && 1'000_u64 == 1000u &&
                   16_usize == usize{16} && static_cast<f64>(2.5_f64) == 2.5 && static_cast<f32>(5_f32) == 5.0f;
        }
        static_assert(scalar_literal_smoke_test());
    } // namespace Detail

} // namespace SFT::Foundation

export namespace SFT {

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
