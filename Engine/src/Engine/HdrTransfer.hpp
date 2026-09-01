#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Engine::Detail {

    /// The subset of ITU-T H.273 transfer characteristics this engine can actually convert to
    /// linear light. H.273 code points are the same ones `cICP`/AVIF's `transferCharacteristics`/
    /// JPEG XL's color encoding use, so this one enum and `transfer_function_from_h273` cover all
    /// of them rather than each decoder re-deriving its own mapping.
    enum class TransferFunction : u8 {
        Srgb,        // Includes BT.709 (code 1) and sRGB (code 13) -- close enough to treat alike here.
        Linear,      // Code 8.
        Pq,          // Code 16, SMPTE ST 2084.
        Hlg,         // Code 18, ARIB STD-B67.
        Unsupported, // Anything else: a real curve this engine doesn't implement.
    };

    /// Maps an ITU-T H.273 `transfer_characteristics` code point to the subset this engine
    /// converts to linear light.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] TransferFunction transfer_function_from_h273(u8 code) noexcept;

    /// Converts a normalized (0-1) SMPTE ST 2084 (PQ) code value to scene-linear light, with the
    /// engine's convention of 1.0 == 100 cd/m^2 reference white (matching the common ACES/OCIO
    /// default for grading against SDR content, since this engine has no configurable reference
    /// white of its own yet).
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 pq_eotf_to_linear(f32 normalized) noexcept;

    /// Converts a normalized (0-1) ARIB STD-B67 (HLG) code value to scene-linear light via HLG's
    /// inverse OETF only. Real HLG display rendering also applies a luminance-dependent system-
    /// gamma OOTF that depends on the target display's peak nits, which this engine has no
    /// pipeline to supply -- omitted here, same "decode correctly, defer real tone-mapping
    /// pipeline integration" stance as this file's EXR/Cineon conversions. The inverse-OETF-only
    /// result is still scene-referred linear light and correct up to that missing display scaling.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 hlg_eotf_to_linear(f32 normalized) noexcept;

    /// Converts an IEEE 754 single-precision float to an IEEE 754 binary16 (half float), rounding
    /// to nearest and flushing values outside half's range to +-infinity/zero as appropriate.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] u16 float_to_half(f32 value) noexcept;

    /// Converts an IEEE 754 binary16 (half float) to a single-precision float.
    ///
    /// @note This function does not throw exceptions.
    [[nodiscard]] f32 half_to_float(u16 value) noexcept;

} // namespace SFT::Engine::Detail
