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

    // Fixed-width scalar aliases used everywhere in the engine — short, explicit, and size-guaranteed
    // (the `static_assert`s below enforce the sizes, and `f32`/`f64` are required to be IEEE-754).
    // Prefer these over `int`/`unsigned`/`float` so widths never drift across platforms. Also exported
    // unqualified into the `SFT` namespace at the bottom of this file for terse use engine-wide.
    using i8 = int8_t;   // signed 8-bit
    using i16 = int16_t; // signed 16-bit
    using i32 = int32_t; // signed 32-bit
    using i64 = int64_t; // signed 64-bit

    using u8 = uint8_t;   // unsigned 8-bit
    using u16 = uint16_t; // unsigned 16-bit
    using u32 = uint32_t; // unsigned 32-bit
    using u64 = uint64_t; // unsigned 64-bit

    using f32 = float;  // 32-bit IEEE-754
    using f64 = double; // 64-bit IEEE-754

    using byte = ::byte;      // raw byte (`std::byte`)
    using usize = size_t;     // unsigned size / index type
    using isize = ptrdiff_t;  // signed size / pointer-difference type

    // A **guaranteed 1-byte boolean**. `bool`'s size is implementation-defined, which makes it unsafe in
    // GPU-facing structs and serialized layouts; `b8` is exactly one byte (`static_assert`ed), trivially
    // copyable, and standard-layout, so it can sit in a struct shared with a shader. Converts to/from
    // `bool` implicitly enough to use like one: `b8 flag = true; if (flag) ...`.
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

    // Runtime re-check of every scalar-size / IEEE-754 / `b8`-layout invariant (the same ones the
    // `static_assert`s above enforce at compile time). Runs automatically once in debug builds via the
    // `Detail::type_assumptions_checked` initializer; also callable directly.
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

    // User-defined literal suffixes are exported globally below, so importing `Sturdy.Foundation` makes
    // `200_u8`, `0xFF_u32`, `16_usize`, and `2.5_f64` available without a `using namespace` directive.
    // `SFT::Foundation::Literals` remains as a compatibility namespace that aliases the same operators.

} // namespace SFT::Foundation

export [[nodiscard]] constexpr SFT::Foundation::i8 operator""_i8(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i8>(v); }
export [[nodiscard]] constexpr SFT::Foundation::i16 operator""_i16(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i16>(v); }
export [[nodiscard]] constexpr SFT::Foundation::i32 operator""_i32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i32>(v); }
export [[nodiscard]] constexpr SFT::Foundation::i64 operator""_i64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::i64>(v); }
export [[nodiscard]] constexpr SFT::Foundation::u8 operator""_u8(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u8>(v); }
export [[nodiscard]] constexpr SFT::Foundation::u16 operator""_u16(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u16>(v); }
export [[nodiscard]] constexpr SFT::Foundation::u32 operator""_u32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u32>(v); }
export [[nodiscard]] constexpr SFT::Foundation::u64 operator""_u64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::u64>(v); }
export [[nodiscard]] constexpr SFT::Foundation::usize operator""_usize(unsigned long long v) noexcept { return static_cast<SFT::Foundation::usize>(v); }
export [[nodiscard]] constexpr SFT::Foundation::isize operator""_isize(unsigned long long v) noexcept { return static_cast<SFT::Foundation::isize>(v); }
export [[nodiscard]] constexpr SFT::Foundation::f32 operator""_f32(long double v) noexcept { return static_cast<SFT::Foundation::f32>(v); }
export [[nodiscard]] constexpr SFT::Foundation::f32 operator""_f32(unsigned long long v) noexcept { return static_cast<SFT::Foundation::f32>(v); }
export [[nodiscard]] constexpr SFT::Foundation::f64 operator""_f64(long double v) noexcept { return static_cast<SFT::Foundation::f64>(v); }
export [[nodiscard]] constexpr SFT::Foundation::f64 operator""_f64(unsigned long long v) noexcept { return static_cast<SFT::Foundation::f64>(v); }

export namespace SFT::Foundation {

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
