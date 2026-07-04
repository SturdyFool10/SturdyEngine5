// `Sturdy.Foundation` — the engine's zero-dependency base layer.
//
// Everything below the renderer/platform stack builds on this: fixed-width scalar types, extended-
// precision integers and floats (`u128`…`f256`), a constexpr math library that works on all of them,
// math constants, concepts, logging, and the mimalloc-backed allocator. It imports nothing from the
// rest of the engine, so any layer can depend on it freely.
//
// This primary module interface re-exports every partition, so a single `import Sturdy.Foundation;`
// brings the whole layer into scope. Numeric and boolean type aliases plus their literal suffixes are
// exported into global scope for terse engine code; `ustr`/`UString` and their slice DSL follow the
// same basic-type pattern while the rest stays under `SFT::Foundation::`.
export module Sturdy.Foundation;

#pragma region Imports
export import :Concepts;
export import :Constants;
export import :Embed;
export import :Log;
export import :Math;
export import :Memory;
export import :NumericConcepts;
export import :Types;
export import :UString;
export import :Utils;
export import :Wide;
export import :WideTraits;
#pragma endregion

// Global basic aliases. These intentionally mirror the aliases in `SFT` so engine code can write `u32`,
// `f128`, `b8`, `ustr`, `UString`, or `uslice(0).to(5)` directly after importing Foundation.
export using b8 [[maybe_unused]] = SFT::Foundation::b8;
export using f32 [[maybe_unused]] = SFT::Foundation::f32;
export using f64 [[maybe_unused]] = SFT::Foundation::f64;
export using f128 [[maybe_unused]] = SFT::Foundation::f128;
export using f256 [[maybe_unused]] = SFT::Foundation::f256;
export using i8 [[maybe_unused]] = SFT::Foundation::i8;
export using i16 [[maybe_unused]] = SFT::Foundation::i16;
export using i32 [[maybe_unused]] = SFT::Foundation::i32;
export using i64 [[maybe_unused]] = SFT::Foundation::i64;
export using i128 [[maybe_unused]] = SFT::Foundation::i128;
export using i256 [[maybe_unused]] = SFT::Foundation::i256;
export using isize [[maybe_unused]] = SFT::Foundation::isize;
export using u8 [[maybe_unused]] = SFT::Foundation::u8;
export using u16 [[maybe_unused]] = SFT::Foundation::u16;
export using u32 [[maybe_unused]] = SFT::Foundation::u32;
export using u64 [[maybe_unused]] = SFT::Foundation::u64;
export using u128 [[maybe_unused]] = SFT::Foundation::u128;
export using u256 [[maybe_unused]] = SFT::Foundation::u256;
export using usize [[maybe_unused]] = SFT::Foundation::usize;
export using USlice [[maybe_unused]] = SFT::Foundation::USlice;
export using USlicePattern [[maybe_unused]] = SFT::Foundation::USlicePattern;
export using ustr [[maybe_unused]] = SFT::Foundation::ustr;
export using UString [[maybe_unused]] = SFT::Foundation::UString;
export using SFT::Foundation::slice_from;
export using SFT::Foundation::uslice;

// Numeric and `ustr` literal suffixes are exported globally by their partitions and re-exported by
// the primary module imports above.

namespace SFT::Foundation::Detail {

    [[nodiscard]] consteval bool global_numeric_import_smoke_test() noexcept {
        const ::u32 scalar = 128_u32;
        const ::u256 wide = 128_u256;
        const ::f256 hex_float = 0x1.8p+2_f256;
        return scalar == 128u && static_cast<::u64>(wide) == 128u && static_cast<::f64>(hex_float) == 6.0 && sizeof(::b8) == 1;
    }

    static_assert(global_numeric_import_smoke_test());

    // Installs mimalloc as the process allocator the first time this module is loaded, before `main()`
    // runs — so every allocation in the program (including static initializers in other modules) goes
    // through it. `[[maybe_unused]]` because the variable exists only for its initializer's side effect.
    [[maybe_unused]] const bool memory_initialized = []() noexcept {
        Memory::initialize();
        return true;
    }();

} // namespace SFT::Foundation::Detail
