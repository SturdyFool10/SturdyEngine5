#pragma once

#include <Foundation/Foundation.hpp>

#include <array>
#include <optional>

namespace SFT::Engine::Detail {

    /// A CIE 1931 xy chromaticity coordinate — a color's hue and saturation with its brightness
    /// divided out, which is how every color space standard states its primaries and white point.
    struct Chromaticity {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    /// The four chromaticities that fully define an additive RGB color space's gamut: where its
    /// three primaries sit, and what "white" means. Everything else about converting between two
    /// RGB spaces is derived from these eight numbers.
    ///
    /// Deliberately holds chromaticities rather than a precomputed matrix: the matrix depends on
    /// both the source *and* the destination (and on the chromatic adaptation between their white
    /// points), so storing one here would only be correct for a single fixed destination.
    struct ColorPrimaries {
        Chromaticity red;
        Chromaticity green;
        Chromaticity blue;
        Chromaticity white;
    };

    /// A 3x3 matrix over linear-light tristimulus values, stored row-major, so that `m[row * 3 +
    /// column]` and the standards' own `M[i][j]` notation agree.
    ///
    /// Only ever applied to *linear* light. Applying one of these to sRGB-encoded (or PQ, or HLG)
    /// samples is meaningless — the transfer function must be undone first, which is what
    /// `HdrTransfer.hpp` is for.
    struct ColorMatrix3 {
        std::array<f32, 9> m{};

        /// Returns the matrix that leaves every color unchanged.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static ColorMatrix3 identity() noexcept;

        /// Transforms one linear RGB (or XYZ) triple by this matrix.
        ///
        /// @param rgb `rgb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::array<f32, 3> apply(const std::array<f32, 3> &rgb) const noexcept;
    };

    /// Returns the matrix product `lhs * rhs`, i.e. the transform that applies `rhs` first.
    ///
    /// @param lhs `lhs` value used by the operation.
    /// @param rhs `rhs` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorMatrix3 multiply(const ColorMatrix3 &lhs, const ColorMatrix3 &rhs) noexcept;

    /// Returns the inverse of `matrix`, or `std::nullopt` when it is singular. A matrix built from
    /// three distinct, non-collinear primaries never is, so a null result means the caller supplied
    /// a degenerate `ColorPrimaries` rather than that inversion is unsupported.
    ///
    /// @param matrix `matrix` value used by the operation.
    ///
    /// @return Returns the value alternative on success; `std::nullopt` when `matrix` is singular.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::optional<ColorMatrix3> inverse(const ColorMatrix3 &matrix) noexcept;

    /// Maps an ITU-T H.273 `colour_primaries` code point to its chromaticities.
    ///
    /// H.273 is the numbering `cICP`, AVIF's `colorPrimaries`, and JPEG XL all use, so this one
    /// function covers every format in this engine that reports its primaries at all. Codes with
    /// no fixed chromaticities (2 = unspecified, and the reserved values) return `std::nullopt`,
    /// which callers treat as "assume it is already in the working space" rather than as an error.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns the primaries on success; `std::nullopt` when `code` names no fixed gamut.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::optional<ColorPrimaries> primaries_from_h273(u8 code) noexcept;

    /// The primaries this engine works in: BT.709 / sRGB (they share a gamut and white point, and
    /// differ only in transfer function). Every scene texture, light color, and UI color is in
    /// these primaries, and the presentation shaders convert to BT.2020 on the way out for a
    /// PQ/HLG swapchain.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorPrimaries working_space_primaries() noexcept;

    /// Returns the matrix taking linear RGB in `primaries` to CIE XYZ, normalized so that RGB
    /// (1, 1, 1) maps exactly to `primaries.white`'s tristimulus value.
    ///
    /// @param primaries `primaries` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorMatrix3 rgb_to_xyz_matrix(const ColorPrimaries &primaries) noexcept;

    /// Returns the matrix taking CIE XYZ to linear RGB in `primaries` — the inverse of
    /// `rgb_to_xyz_matrix`, returning identity for degenerate primaries that cannot be inverted.
    ///
    /// @param primaries `primaries` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorMatrix3 xyz_to_rgb_matrix(const ColorPrimaries &primaries) noexcept;

    /// Returns the Bradford chromatic adaptation matrix taking XYZ measured under illuminant
    /// `from_white` to XYZ as it would have been measured under `to_white`.
    ///
    /// Without this, converting between two spaces with different white points (DCI-P3's ~6300K
    /// theater white against sRGB's D65, say) leaves a visible color cast, because the raw
    /// primary-derived matrices assume both ends agree on what white is. Bradford is the
    /// transform ICC profiles and every color-management library default to; it models adaptation
    /// as an independent gain on three cone-like response channels.
    ///
    /// @param from_white `from_white` value used by the operation.
    /// @param to_white `to_white` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorMatrix3 chromatic_adaptation_matrix(Chromaticity from_white,
                                                           Chromaticity to_white) noexcept;

    /// Returns the single matrix converting linear RGB in `from` to linear RGB in `to`, including
    /// the Bradford adaptation between their white points. This is the whole input color transform
    /// collapsed into one 3x3 that a decode loop can apply per pixel.
    ///
    /// @param from `from` value used by the operation.
    /// @param to `to` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] ColorMatrix3 color_conversion_matrix(const ColorPrimaries &from,
                                                       const ColorPrimaries &to) noexcept;

    /// What to do with a color that converted out of the destination gamut. Converting a wide-gamut
    /// image into narrower primaries always produces some of these: a saturated BT.2020 green
    /// simply has no sRGB coordinate, and comes out with a negative red component.
    enum class GamutMapping : u8 {
        /// Clamp each channel independently. Cheapest, and what most naive pipelines do, but it
        /// shifts hue — clamping only the negative red of an out-of-gamut cyan moves the color
        /// toward blue rather than just desaturating it.
        Clip,
        /// Desaturate the color toward its own luminance by the smallest amount that brings every
        /// channel into range. Preserves hue and (as far as clipping allows) brightness, so an
        /// out-of-gamut green becomes a less saturated green of the same lightness rather than a
        /// different hue. The default, and what makes wide-gamut sources look right rather than
        /// merely not-broken.
        Desaturate,
    };

    /// Brings `rgb` (linear light, in the destination primaries) into `[0, maximum]` on every
    /// channel using `mapping`.
    ///
    /// `maximum` is the largest value a channel is allowed to reach. Pass infinity to fix only the
    /// negative channels — that is, to correct the color's *chromaticity* without touching its
    /// brightness. Only pass a finite ceiling when a value above it genuinely is a gamut artifact
    /// rather than a real highlight; compressing scene-referred HDR brightness this way desaturates
    /// highlights toward white, which is tone mapping's job and not this function's.
    ///
    /// @param rgb `rgb` value used by the operation.
    /// @param mapping `mapping` value used by the operation.
    /// @param maximum Largest representable channel value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::array<f32, 3> map_into_gamut(const std::array<f32, 3> &rgb,
                                                    GamutMapping mapping,
                                                    f32 maximum) noexcept;

} // namespace SFT::Engine::Detail
