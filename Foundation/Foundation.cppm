module;

#include "Foundation/Concepts.hpp"
#include "Foundation/Constants.hpp"
#include "Foundation/Embed.hpp"
#include "Foundation/Log.hpp"
#include "Foundation/Math.hpp"
#include "Foundation/Memory.hpp"
#include "Foundation/NumericConcepts.hpp"
#include "Foundation/Types.hpp"
#include "Foundation/Wide.hpp"
#include "Foundation/WideTraits.hpp"

export module Sturdy.Foundation;

namespace SFT::Foundation::Detail {

    [[maybe_unused]] const bool memory_initialized = []() noexcept {
        Memory::initialize();
        return true;
    }();

} // namespace SFT::Foundation::Detail

namespace SFT::Foundation {
    export using ::SFT::Foundation::b8;
    export using ::SFT::Foundation::byte;
    export using ::SFT::Foundation::f32;
    export using ::SFT::Foundation::f64;
    export using ::SFT::Foundation::i16;
    export using ::SFT::Foundation::i32;
    export using ::SFT::Foundation::i64;
    export using ::SFT::Foundation::i8;
    export using ::SFT::Foundation::isize;
    export using ::SFT::Foundation::log_debug;
    export using ::SFT::Foundation::log_error;
    export using ::SFT::Foundation::log_info;
    export using ::SFT::Foundation::log_trace;
    export using ::SFT::Foundation::log_warn;
    export using ::SFT::Foundation::u16;
    export using ::SFT::Foundation::u32;
    export using ::SFT::Foundation::u64;
    export using ::SFT::Foundation::u8;
    export using ::SFT::Foundation::usize;

    // Public concepts (Concepts.hpp / NumericConcepts.hpp).
    export using ::SFT::Foundation::AssignableFrom;
    export using ::SFT::Foundation::Boolean;
    export using ::SFT::Foundation::ConstructibleFrom;
    export using ::SFT::Foundation::CopyConstructible;
    export using ::SFT::Foundation::Copyable;
    export using ::SFT::Foundation::DefaultConstructible;
    export using ::SFT::Foundation::Destructible;
    export using ::SFT::Foundation::Enum;
    export using ::SFT::Foundation::EqualityComparable;
    export using ::SFT::Foundation::EqualityComparableWith;
    export using ::SFT::Foundation::Float;
    export using ::SFT::Foundation::Hashable;
    export using ::SFT::Foundation::Integer;
    export using ::SFT::Foundation::Movable;
    export using ::SFT::Foundation::MoveConstructible;
    export using ::SFT::Foundation::Number;
    export using ::SFT::Foundation::Object;
    export using ::SFT::Foundation::Regular;
    export using ::SFT::Foundation::Semiregular;
    export using ::SFT::Foundation::Swappable;
    export using ::SFT::Foundation::SwappableWith;
    export using ::SFT::Foundation::ThreeWayComparable;
    export using ::SFT::Foundation::ThreeWayComparableWith;
    export using ::SFT::Foundation::TotallyOrdered;
    export using ::SFT::Foundation::TotallyOrderedWith;
    export using ::SFT::Foundation::UnsignedInteger;

    // Wide numeric types (Wide.hpp).
    export using ::SFT::Foundation::f128;
    export using ::SFT::Foundation::f256;
    export using ::SFT::Foundation::i128;
    export using ::SFT::Foundation::i256;
    export using ::SFT::Foundation::u128;
    export using ::SFT::Foundation::u256;

    // Math functions, uniform across regular and wide types (Math.hpp). Kept in SFT::Foundation
    // only (not bare SFT) — names like abs/floor/sqrt collide with std and the global namespace.
    export using ::SFT::Foundation::abs;
    export using ::SFT::Foundation::acos;
    export using ::SFT::Foundation::asin;
    export using ::SFT::Foundation::atan;
    export using ::SFT::Foundation::atan2;
    export using ::SFT::Foundation::cbrt;
    export using ::SFT::Foundation::ceil;
    export using ::SFT::Foundation::copysign;
    export using ::SFT::Foundation::cos;
    export using ::SFT::Foundation::cosh;
    export using ::SFT::Foundation::exp;
    export using ::SFT::Foundation::fma;
    export using ::SFT::Foundation::floor;
    export using ::SFT::Foundation::fmod;
    export using ::SFT::Foundation::fract;
    export using ::SFT::Foundation::hypot;
    export using ::SFT::Foundation::is_finite;
    export using ::SFT::Foundation::is_inf;
    export using ::SFT::Foundation::is_nan;
    export using ::SFT::Foundation::isfinite;
    export using ::SFT::Foundation::isinf;
    export using ::SFT::Foundation::isnan;
    export using ::SFT::Foundation::ldexp;
    export using ::SFT::Foundation::lerp;
    export using ::SFT::Foundation::log;
    export using ::SFT::Foundation::log2;
    export using ::SFT::Foundation::log10;
    export using ::SFT::Foundation::pow;
    export using ::SFT::Foundation::remainder;
    export using ::SFT::Foundation::round;
    export using ::SFT::Foundation::saturate;
    export using ::SFT::Foundation::scalbn;
    export using ::SFT::Foundation::sign;
    export using ::SFT::Foundation::signbit;
    export using ::SFT::Foundation::sin;
    export using ::SFT::Foundation::sinh;
    export using ::SFT::Foundation::sqrt;
    export using ::SFT::Foundation::tan;
    export using ::SFT::Foundation::tanh;
    export using ::SFT::Foundation::trunc;

    // Math constants (Constants.hpp).
    export using ::SFT::Foundation::apery;
    export using ::SFT::Foundation::catalan;
    export using ::SFT::Foundation::conway;
    export using ::SFT::Foundation::deg_to_rad;
    export using ::SFT::Foundation::e;
    export using ::SFT::Foundation::euler_mascheroni;
    export using ::SFT::Foundation::feigenbaum_alpha;
    export using ::SFT::Foundation::feigenbaum_delta;
    export using ::SFT::Foundation::four_over_pi;
    export using ::SFT::Foundation::four_pi;
    export using ::SFT::Foundation::glaisher_kinkelin;
    export using ::SFT::Foundation::grad_to_rad;
    export using ::SFT::Foundation::half_pi;
    export using ::SFT::Foundation::inv_e;
    export using ::SFT::Foundation::inv_phi;
    export using ::SFT::Foundation::inv_pi;
    export using ::SFT::Foundation::inv_sqrt_pi;
    export using ::SFT::Foundation::inv_tau;
    export using ::SFT::Foundation::khinchin;
    export using ::SFT::Foundation::log10_e;
    export using ::SFT::Foundation::log2_e;
    export using ::SFT::Foundation::omega;
    export using ::SFT::Foundation::one;
    export using ::SFT::Foundation::phi;
    export using ::SFT::Foundation::phi_squared;
    export using ::SFT::Foundation::pi;
    export using ::SFT::Foundation::pi_cubed;
    export using ::SFT::Foundation::pi_squared;
    export using ::SFT::Foundation::plastic;
    export using ::SFT::Foundation::quarter_pi;
    export using ::SFT::Foundation::rad_to_deg;
    export using ::SFT::Foundation::rad_to_grad;
    export using ::SFT::Foundation::silver_ratio;
    export using ::SFT::Foundation::sqrt_half_pi;
    export using ::SFT::Foundation::sqrt_pi;
    export using ::SFT::Foundation::sqrt_tau;
    export using ::SFT::Foundation::tau;
    export using ::SFT::Foundation::third_pi;
    export using ::SFT::Foundation::three_pi;
    export using ::SFT::Foundation::two_over_pi;
    export using ::SFT::Foundation::two_pi;
    export using ::SFT::Foundation::zero;
} // namespace SFT::Foundation

namespace SFT::Foundation::Literals {
    // Wide numeric literals (Wide.hpp). Opt in per scope with `using namespace
    // SFT::Foundation::Literals;` to write u128/i128/u256/i256/f128/f256 values inline.
    export using ::SFT::Foundation::Literals::operator""_f128;
    export using ::SFT::Foundation::Literals::operator""_f256;
    export using ::SFT::Foundation::Literals::operator""_i128;
    export using ::SFT::Foundation::Literals::operator""_i256;
    export using ::SFT::Foundation::Literals::operator""_u128;
    export using ::SFT::Foundation::Literals::operator""_u256;

    // literals for normal numbers
    export using ::SFT::Foundation::Literals::operator""_f64;
    export using ::SFT::Foundation::Literals::operator""_i64;
    export using ::SFT::Foundation::Literals::operator""_u64;
    export using ::SFT::Foundation::Literals::operator""_f32;
    export using ::SFT::Foundation::Literals::operator""_i32;
    export using ::SFT::Foundation::Literals::operator""_u32;
    export using ::SFT::Foundation::Literals::operator""_i16;
    export using ::SFT::Foundation::Literals::operator""_u16;
} // namespace SFT::Foundation::Literals

namespace SFT {
    export using ::SFT::b8;
    export using ::SFT::byte;
    export using ::SFT::f32;
    export using ::SFT::f64;
    export using ::SFT::i8;
    export using ::SFT::i16;
    export using ::SFT::i32;
    export using ::SFT::i64;
    export using ::SFT::isize;
    export using ::SFT::u8;
    export using ::SFT::u16;
    export using ::SFT::u32;
    export using ::SFT::u64;
    export using ::SFT::usize;

    export using ::SFT::f128;
    export using ::SFT::f256;
    export using ::SFT::i128;
    export using ::SFT::i256;
    export using ::SFT::u128;
    export using ::SFT::u256;
} // namespace SFT

namespace SFT::Foundation::Memory {
    // Thin mimalloc wrapper (Memory.hpp). Foundation initializes mimalloc as the module is loaded;
    // these exports expose the explicit allocator hooks for subsystems that want them.
    export using ::SFT::Foundation::Memory::ByteFormatOptions;
    export using ::SFT::Foundation::Memory::ByteUnit;
    export using ::SFT::Foundation::Memory::HeapUsage;
    export using ::SFT::Foundation::Memory::allocate;
    export using ::SFT::Foundation::Memory::allocate_aligned;
    export using ::SFT::Foundation::Memory::allocate_zeroed;
    export using ::SFT::Foundation::Memory::allocate_zeroed_aligned;
    export using ::SFT::Foundation::Memory::collect;
    export using ::SFT::Foundation::Memory::bytes_as;
    export using ::SFT::Foundation::Memory::deallocate;
    export using ::SFT::Foundation::Memory::format_bytes;
    export using ::SFT::Foundation::Memory::format_heap_bytes;
    export using ::SFT::Foundation::Memory::good_size;
    export using ::SFT::Foundation::Memory::heap_bytes;
    export using ::SFT::Foundation::Memory::heap_usage;
    export using ::SFT::Foundation::Memory::initialize;
    export using ::SFT::Foundation::Memory::is_initialized;
    export using ::SFT::Foundation::Memory::log_stats;
    export using ::SFT::Foundation::Memory::merge_thread_stats;
    export using ::SFT::Foundation::Memory::mimalloc_version;
    export using ::SFT::Foundation::Memory::peak_heap_bytes;
    export using ::SFT::Foundation::Memory::reallocate;
    export using ::SFT::Foundation::Memory::reallocate_aligned;
    export using ::SFT::Foundation::Memory::reset_stats;
    export using ::SFT::Foundation::Memory::usable_size;
} // namespace SFT::Foundation::Memory
