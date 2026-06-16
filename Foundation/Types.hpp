#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace SFT::Foundation {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

using byte = std::byte;
using usize = std::size_t;
using isize = std::ptrdiff_t;

class b8 {
public:
    constexpr b8() noexcept = default;
    constexpr b8(bool value) noexcept
        : value_(value ? u8 {1} : u8 {0})
    {
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return value_ != 0;
    }

    [[nodiscard]] constexpr u8 value() const noexcept
    {
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

static_assert(std::numeric_limits<f32>::is_iec559, "f32 must be IEEE-754.");
static_assert(std::numeric_limits<f64>::is_iec559, "f64 must be IEEE-754.");
static_assert(std::is_trivially_copyable_v<b8>);
static_assert(std::is_standard_layout_v<b8>);

inline void assert_type_assumptions() noexcept
{
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

    assert(std::numeric_limits<f32>::is_iec559);
    assert(std::numeric_limits<f64>::is_iec559);
    assert(std::is_trivially_copyable_v<b8>);
    assert(std::is_standard_layout_v<b8>);
}

namespace Detail {

#if !defined(NDEBUG)
[[maybe_unused]] inline const bool type_assumptions_checked = []() noexcept {
    assert_type_assumptions();
    return true;
}();
#endif

} // namespace Detail

} // namespace SFT::Foundation

namespace SFT {

using Foundation::b8;
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

} // namespace SFT
