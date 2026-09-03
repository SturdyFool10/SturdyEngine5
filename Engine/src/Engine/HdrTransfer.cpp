#include <Engine/HdrTransfer.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace SFT::Engine::Detail {

    namespace {

        // SMPTE ST 2084 constants, in the spec's own naming. Written as expressions over the
        // exact rational values the spec gives rather than pre-rounded decimals, so the
        // compile-time folding is as accurate as f32 allows.
        constexpr f32 kPqM1 = 2610.0f / 16384.0f;
        constexpr f32 kPqM2 = 2523.0f / 4096.0f * 128.0f;
        constexpr f32 kPqC1 = 3424.0f / 4096.0f;
        constexpr f32 kPqC2 = 2413.0f / 4096.0f * 32.0f;
        constexpr f32 kPqC3 = 2392.0f / 4096.0f * 32.0f;

        /// PQ's EOTF is defined against a 10000 cd/m^2 peak, but this engine's linear convention
        /// is 1.0 == 100 cd/m^2 (see `pq_eotf_to_linear`'s declaration), so the spec result is
        /// scaled by this.
        constexpr f32 kPqPeakNits = 10000.0f;
        constexpr f32 kEngineReferenceWhiteNits = 100.0f;

        // ARIB STD-B67 (HLG) inverse-OETF constants, from the standard's Table 5.
        constexpr f32 kHlgA = 0.17883277f;
        constexpr f32 kHlgB = 1.0f - 4.0f * kHlgA; // 0.28466892
        // Defined by the standard as 0.5 - a * ln(4a); written as the standard's own rounded
        // decimal because std::log is not usable in a constant expression.
        constexpr f32 kHlgC = 0.55991073f;

    } // namespace

    /// Maps an ITU-T H.273 `transfer_characteristics` code point to the subset this engine
    /// converts to linear light.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    TransferFunction transfer_function_from_h273(u8 code) noexcept {
        switch (code) {
            // 1 (BT.709), 6 (BT.601), 14/15 (BT.2020 10/12-bit) all share the same 1.099/0.018
            // camera OETF, which differs from sRGB's curve only below ~0.018 and by less than one
            // 8-bit code value anywhere -- not worth a separate branch at this engine's precision.
            case 1:
            case 6:
            case 13:
            case 14:
            case 15:
                return TransferFunction::Srgb;
            case 8:
                return TransferFunction::Linear;
            case 16:
                return TransferFunction::Pq;
            case 18:
                return TransferFunction::Hlg;
            default:
                return TransferFunction::Unsupported;
        }
    }

    /// Converts a normalized (0-1) SMPTE ST 2084 (PQ) code value to scene-linear light.
    ///
    /// @param normalized `normalized` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 pq_eotf_to_linear(f32 normalized) noexcept {
        const f32 clamped = std::clamp(normalized, 0.0f, 1.0f);
        const f32 encoded = std::pow(clamped, 1.0f / kPqM2);
        // The spec's numerator is max(encoded - c1, 0): below c1 the signal is at or under
        // absolute black, and without the clamp std::pow of a negative base returns NaN.
        const f32 numerator = std::max(encoded - kPqC1, 0.0f);
        const f32 denominator = kPqC2 - kPqC3 * encoded;
        if (denominator <= 0.0f) {
            return 0.0f;
        }
        const f32 display_nits = std::pow(numerator / denominator, 1.0f / kPqM1) * kPqPeakNits;
        return display_nits / kEngineReferenceWhiteNits;
    }

    /// Converts a normalized (0-1) ARIB STD-B67 (HLG) code value to scene-linear light via HLG's
    /// inverse OETF only.
    ///
    /// @param normalized `normalized` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 hlg_eotf_to_linear(f32 normalized) noexcept {
        const f32 clamped = std::clamp(normalized, 0.0f, 1.0f);
        if (clamped <= 0.5f) {
            return clamped * clamped / 3.0f;
        }
        return (std::exp((clamped - kHlgC) / kHlgA) + kHlgB) / 12.0f;
    }

    /// Converts an IEEE 754 single-precision float to an IEEE 754 binary16 (half float).
    ///
    /// @param value `value` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u16 float_to_half(f32 value) noexcept {
        const u32 bits = std::bit_cast<u32>(value);
        const u16 sign = static_cast<u16>((bits >> 16) & 0x8000u);
        // Biased single exponent (127) rebiased to half's (15); may go negative, hence i32.
        i32 exponent = static_cast<i32>((bits >> 23) & 0xFFu) - 127 + 15;
        u32 mantissa = bits & 0x007FFFFFu;

        if (((bits >> 23) & 0xFFu) == 0xFFu) {
            // Infinity stays infinity; any NaN maps to a quiet NaN rather than to infinity, which
            // is what dropping the mantissa outright would silently produce.
            return static_cast<u16>(sign | 0x7C00u | (mantissa != 0 ? 0x0200u : 0u));
        }
        if (exponent >= 0x1F) {
            return static_cast<u16>(sign | 0x7C00u); // Overflows half's range: flush to infinity.
        }
        if (exponent <= 0) {
            if (exponent < -10) {
                return sign; // Smaller than the smallest half subnormal: flush to zero.
            }
            // Subnormal: restore the implicit leading 1 and shift it down into the subnormal range,
            // rounding to nearest-even on the bits shifted out.
            mantissa |= 0x00800000u;
            const u32 shift = static_cast<u32>(14 - exponent);
            const u32 shifted = mantissa >> shift;
            const u32 remainder = mantissa & ((1u << shift) - 1u);
            const u32 halfway = 1u << (shift - 1);
            u32 rounded = shifted;
            if (remainder > halfway || (remainder == halfway && (shifted & 1u) != 0)) {
                ++rounded; // May carry into the exponent field, which is the correct result.
            }
            return static_cast<u16>(sign | rounded);
        }

        // Normal: round the 23-bit mantissa to 10 bits, nearest-even.
        const u32 remainder = mantissa & 0x00001FFFu;
        u16 result = static_cast<u16>(sign | (static_cast<u32>(exponent) << 10) | (mantissa >> 13));
        if (remainder > 0x1000u || (remainder == 0x1000u && ((mantissa >> 13) & 1u) != 0)) {
            // Incrementing the combined exponent+mantissa field carries correctly into the
            // exponent, and on to 0x7C00 (infinity) when the exponent itself overflows.
            ++result;
        }
        return result;
    }

    /// Converts an IEEE 754 binary16 (half float) to a single-precision float.
    ///
    /// @param value `value` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    f32 half_to_float(u16 value) noexcept {
        const u32 sign = static_cast<u32>(value & 0x8000u) << 16;
        const u32 exponent = (value >> 10) & 0x1Fu;
        const u32 mantissa = value & 0x03FFu;

        if (exponent == 0) {
            if (mantissa == 0) {
                return std::bit_cast<f32>(sign); // Signed zero.
            }
            // Subnormal half: normalize by shifting the mantissa up until its leading 1 falls out
            // of the 10-bit field, decrementing the exponent to match.
            u32 shifted_mantissa = mantissa;
            i32 shifted_exponent = -1;
            do {
                shifted_mantissa <<= 1;
                ++shifted_exponent;
            } while ((shifted_mantissa & 0x0400u) == 0);
            shifted_mantissa &= 0x03FFu;
            const u32 single_exponent = static_cast<u32>(127 - 15 - shifted_exponent);
            return std::bit_cast<f32>(sign | (single_exponent << 23) | (shifted_mantissa << 13));
        }
        if (exponent == 0x1F) {
            return std::bit_cast<f32>(sign | 0x7F800000u | (mantissa << 13)); // Infinity or NaN.
        }
        return std::bit_cast<f32>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
    }

} // namespace SFT::Engine::Detail
