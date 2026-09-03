#include <Engine/ColorSpace.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Engine::Detail {

    namespace {

        /// The Bradford cone response matrix (XYZ to the "LMS"-like space adaptation is modelled
        /// in), from the CIECAM97s-derived form that ICC profiles and every color-management
        /// library use. Its inverse is computed rather than tabulated so the two cannot drift.
        constexpr ColorMatrix3 kBradford{{
            0.8951f, 0.2664f, -0.1614f,
            -0.7502f, 1.7135f, 0.0367f,
            0.0389f, -0.0685f, 1.0296f,
        }};

        /// Converts an xy chromaticity to the XYZ tristimulus of a color with that chromaticity and
        /// unit luminance (Y = 1) — the form white points are needed in throughout this file.
        ///
        /// @param chromaticity `chromaticity` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::array<f32, 3> chromaticity_to_xyz(Chromaticity chromaticity) noexcept {
            if (chromaticity.y == 0.0f) {
                // A zero y is not a real chromaticity (it would be a color of zero luminance at
                // nonzero brightness); returning black keeps the derived matrices finite instead of
                // propagating infinities out of a malformed input.
                return {0.0f, 0.0f, 0.0f};
            }
            return {
                chromaticity.x / chromaticity.y,
                1.0f,
                (1.0f - chromaticity.x - chromaticity.y) / chromaticity.y,
            };
        }

        /// Builds a matrix whose columns are the three supplied vectors.
        ///
        /// @param c0 `c0` value used by the operation.
        /// @param c1 `c1` value used by the operation.
        /// @param c2 `c2` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ColorMatrix3 from_columns(const std::array<f32, 3> &c0,
                                                const std::array<f32, 3> &c1,
                                                const std::array<f32, 3> &c2) noexcept {
            return ColorMatrix3{{
                c0[0], c1[0], c2[0],
                c0[1], c1[1], c2[1],
                c0[2], c1[2], c2[2],
            }};
        }

    } // namespace

    /// Returns the matrix that leaves every color unchanged.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 ColorMatrix3::identity() noexcept {
        return ColorMatrix3{{
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
        }};
    }

    /// Transforms one linear RGB (or XYZ) triple by this matrix.
    ///
    /// @param rgb `rgb` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    std::array<f32, 3> ColorMatrix3::apply(const std::array<f32, 3> &rgb) const noexcept {
        return {
            m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2],
            m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2],
            m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2],
        };
    }

    /// Returns the matrix product `lhs * rhs`.
    ///
    /// @param lhs `lhs` value used by the operation.
    /// @param rhs `rhs` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 multiply(const ColorMatrix3 &lhs, const ColorMatrix3 &rhs) noexcept {
        ColorMatrix3 result{};
        for (usize row = 0; row < 3; ++row) {
            for (usize column = 0; column < 3; ++column) {
                f32 sum = 0.0f;
                for (usize k = 0; k < 3; ++k) {
                    sum += lhs.m[row * 3 + k] * rhs.m[k * 3 + column];
                }
                result.m[row * 3 + column] = sum;
            }
        }
        return result;
    }

    /// Returns the inverse of `matrix`, or `std::nullopt` when it is singular.
    ///
    /// @param matrix `matrix` value used by the operation.
    ///
    /// @return Returns the value alternative on success; `std::nullopt` when `matrix` is singular.
    /// @note This function does not throw exceptions.
    std::optional<ColorMatrix3> inverse(const ColorMatrix3 &matrix) noexcept {
        const auto &m = matrix.m;
        const f32 cofactor0 = m[4] * m[8] - m[5] * m[7];
        const f32 cofactor1 = m[5] * m[6] - m[3] * m[8];
        const f32 cofactor2 = m[3] * m[7] - m[4] * m[6];
        const f32 determinant = m[0] * cofactor0 + m[1] * cofactor1 + m[2] * cofactor2;
        // Compared against an absolute epsilon rather than exactly zero: the determinants here come
        // out of chromaticity arithmetic, so a degenerate set of primaries lands near zero rather
        // than on it, and dividing by that produces garbage just as surely.
        if (std::abs(determinant) < 1e-12f) {
            return std::nullopt;
        }
        const f32 inverse_determinant = 1.0f / determinant;
        return ColorMatrix3{{
            cofactor0 * inverse_determinant,
            (m[2] * m[7] - m[1] * m[8]) * inverse_determinant,
            (m[1] * m[5] - m[2] * m[4]) * inverse_determinant,
            cofactor1 * inverse_determinant,
            (m[0] * m[8] - m[2] * m[6]) * inverse_determinant,
            (m[2] * m[3] - m[0] * m[5]) * inverse_determinant,
            cofactor2 * inverse_determinant,
            (m[1] * m[6] - m[0] * m[7]) * inverse_determinant,
            (m[0] * m[4] - m[1] * m[3]) * inverse_determinant,
        }};
    }

    /// Maps an ITU-T H.273 `colour_primaries` code point to its chromaticities.
    ///
    /// @param code `code` value used by the operation.
    ///
    /// @return Returns the primaries on success; `std::nullopt` when `code` names no fixed gamut.
    /// @note This function does not throw exceptions.
    std::optional<ColorPrimaries> primaries_from_h273(u8 code) noexcept {
        // Chromaticities are H.273 Table 2 verbatim. D65 is written as (0.3127, 0.3290) throughout,
        // which is the value every one of these standards states -- deliberately not the more
        // precise (0.31272, 0.32903) some references give, so that a matrix derived here matches
        // one derived from the published tables.
        constexpr Chromaticity kD65{0.3127f, 0.3290f};
        switch (code) {
            case 1: // BT.709, and sRGB/sYCC, which share its gamut exactly.
                return ColorPrimaries{{0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, kD65};
            case 4: // BT.470 System M (historical NTSC), illuminant C.
                return ColorPrimaries{
                    {0.67f, 0.33f}, {0.21f, 0.71f}, {0.14f, 0.08f}, {0.3100f, 0.3160f}};
            case 5: // BT.470 System B/G, BT.601 625-line (PAL/SECAM).
                return ColorPrimaries{{0.64f, 0.33f}, {0.29f, 0.60f}, {0.15f, 0.06f}, kD65};
            case 6: // BT.601 525-line / SMPTE 170M.
            case 7: // SMPTE 240M -- identical primaries to 170M; only its transfer curve differs.
                return ColorPrimaries{{0.630f, 0.340f}, {0.310f, 0.595f}, {0.155f, 0.070f}, kD65};
            case 8: // Generic film (colour filters using illuminant C).
                return ColorPrimaries{
                    {0.681f, 0.319f}, {0.243f, 0.692f}, {0.145f, 0.049f}, {0.3100f, 0.3160f}};
            case 9: // BT.2020, and BT.2100 -- the HDR gamut PQ/HLG content is graded in.
                return ColorPrimaries{{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, kD65};
            // 10 is SMPTE ST 428-1, whose samples are CIE XYZ directly rather than an RGB gamut.
            // Its "primaries" are the XYZ axes, two of which have y = 0 and so are not real
            // chromaticities at all -- the generic derivation below cannot express them, and
            // pretending otherwise would silently produce a degenerate matrix. Deliberately
            // unhandled: no decoder in this engine produces XYZ samples (a DCI digital-cinema JPEG
            // 2000 is the only realistic source, and this engine's JP2 path does not read one),
            // so it falls through to "unspecified" rather than carrying a broken special case.
            case 11: // SMPTE RP 431-2 (DCI-P3), with DCI's own theatrical white, not D65.
                return ColorPrimaries{
                    {0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, {0.314f, 0.351f}};
            case 12: // SMPTE EG 432-1 (Display P3): DCI-P3 primaries re-white-pointed to D65.
                return ColorPrimaries{{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, kD65};
            case 22: // EBU Tech 3213-E.
                return ColorPrimaries{{0.630f, 0.340f}, {0.295f, 0.605f}, {0.155f, 0.077f}, kD65};
            default:
                // 0, 3, and 13-21 are reserved; 2 is explicitly "unspecified". None of them name a
                // gamut, so there is nothing to convert from.
                return std::nullopt;
        }
    }

    /// The primaries this engine works in: BT.709 / sRGB.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorPrimaries working_space_primaries() noexcept {
        // H.273 code 1 is BT.709/sRGB by definition, so this cannot be nullopt; the fallback keeps
        // the function total rather than asserting.
        const std::optional<ColorPrimaries> bt709 = primaries_from_h273(1);
        return bt709.value_or(ColorPrimaries{
            {0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, {0.3127f, 0.3290f}});
    }

    /// Returns the matrix taking linear RGB in `primaries` to CIE XYZ.
    ///
    /// @param primaries `primaries` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 rgb_to_xyz_matrix(const ColorPrimaries &primaries) noexcept {
        // Standard derivation: the primaries' chromaticities fix the *direction* of each column,
        // and the white point fixes their relative scaling (since RGB (1,1,1) must land exactly on
        // white). So build the unscaled matrix, solve it for the scale factors that map (1,1,1) to
        // white's XYZ, then scale each column by its factor.
        const std::array<f32, 3> red = chromaticity_to_xyz(primaries.red);
        const std::array<f32, 3> green = chromaticity_to_xyz(primaries.green);
        const std::array<f32, 3> blue = chromaticity_to_xyz(primaries.blue);
        const ColorMatrix3 unscaled = from_columns(red, green, blue);

        const std::optional<ColorMatrix3> unscaled_inverse = inverse(unscaled);
        if (!unscaled_inverse) {
            return ColorMatrix3::identity();
        }
        const std::array<f32, 3> scale =
            unscaled_inverse->apply(chromaticity_to_xyz(primaries.white));

        return from_columns({red[0] * scale[0], red[1] * scale[0], red[2] * scale[0]},
                            {green[0] * scale[1], green[1] * scale[1], green[2] * scale[1]},
                            {blue[0] * scale[2], blue[1] * scale[2], blue[2] * scale[2]});
    }

    /// Returns the matrix taking CIE XYZ to linear RGB in `primaries`.
    ///
    /// @param primaries `primaries` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 xyz_to_rgb_matrix(const ColorPrimaries &primaries) noexcept {
        return inverse(rgb_to_xyz_matrix(primaries)).value_or(ColorMatrix3::identity());
    }

    /// Returns the Bradford chromatic adaptation matrix between two white points.
    ///
    /// @param from_white `from_white` value used by the operation.
    /// @param to_white `to_white` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 chromatic_adaptation_matrix(Chromaticity from_white, Chromaticity to_white) noexcept {
        const std::optional<ColorMatrix3> bradford_inverse = inverse(kBradford);
        if (!bradford_inverse) {
            return ColorMatrix3::identity();
        }

        // Both white points expressed in Bradford cone space; adaptation is then the per-channel
        // gain that takes one to the other, sandwiched back into XYZ.
        const std::array<f32, 3> from_cone = kBradford.apply(chromaticity_to_xyz(from_white));
        const std::array<f32, 3> to_cone = kBradford.apply(chromaticity_to_xyz(to_white));

        ColorMatrix3 gain = ColorMatrix3::identity();
        for (usize i = 0; i < 3; ++i) {
            // A zero source response would be a white point with no stimulus on one cone channel,
            // which no real illuminant has; guarded so a malformed chromaticity cannot produce
            // infinities that then poison every converted pixel.
            gain.m[i * 3 + i] = from_cone[i] != 0.0f ? to_cone[i] / from_cone[i] : 1.0f;
        }

        return multiply(*bradford_inverse, multiply(gain, kBradford));
    }

    /// Returns the single matrix converting linear RGB in `from` to linear RGB in `to`.
    ///
    /// @param from `from` value used by the operation.
    /// @param to `to` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    ColorMatrix3 color_conversion_matrix(const ColorPrimaries &from, const ColorPrimaries &to) noexcept {
        const ColorMatrix3 to_xyz = rgb_to_xyz_matrix(from);
        const ColorMatrix3 adapt = chromatic_adaptation_matrix(from.white, to.white);
        const ColorMatrix3 from_xyz = xyz_to_rgb_matrix(to);
        return multiply(from_xyz, multiply(adapt, to_xyz));
    }

    /// Brings `rgb` into `[0, maximum]` on every channel using `mapping`.
    ///
    /// @param rgb `rgb` value used by the operation.
    /// @param mapping `mapping` value used by the operation.
    /// @param maximum Largest representable channel value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    std::array<f32, 3> map_into_gamut(const std::array<f32, 3> &rgb,
                                      GamutMapping mapping,
                                      f32 maximum) noexcept {
        const auto clip = [maximum](const std::array<f32, 3> &color) {
            return std::array<f32, 3>{
                std::clamp(color[0], 0.0f, maximum),
                std::clamp(color[1], 0.0f, maximum),
                std::clamp(color[2], 0.0f, maximum),
            };
        };
        if (mapping == GamutMapping::Clip) {
            return clip(rgb);
        }

        const bool in_gamut = rgb[0] >= 0.0f && rgb[1] >= 0.0f && rgb[2] >= 0.0f &&
                              rgb[0] <= maximum && rgb[1] <= maximum && rgb[2] <= maximum;
        if (in_gamut) {
            return rgb;
        }

        // Desaturate along the line from the color to its own achromatic luminance. BT.709 luma
        // weights: this runs after conversion, so the color is already in the destination
        // primaries, and the destination is always the working space (see color_conversion_matrix's
        // callers).
        const f32 luminance = std::clamp(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2],
                                         0.0f, maximum);

        // Find the smallest blend toward `luminance` that brings every channel into range. Each
        // out-of-range channel gives a lower bound on the blend factor; the largest wins.
        f32 blend = 0.0f;
        for (usize i = 0; i < 3; ++i) {
            const f32 delta = rgb[i] - luminance;
            if (delta == 0.0f) {
                continue; // Already achromatic on this channel: blending cannot move it.
            }
            if (rgb[i] < 0.0f) {
                blend = std::max(blend, rgb[i] / delta);
            } else if (rgb[i] > maximum) {
                blend = std::max(blend, (rgb[i] - maximum) / delta);
            }
        }
        blend = std::clamp(blend, 0.0f, 1.0f);

        const std::array<f32, 3> desaturated{
            rgb[0] + (luminance - rgb[0]) * blend,
            rgb[1] + (luminance - rgb[1]) * blend,
            rgb[2] + (luminance - rgb[2]) * blend,
        };
        // Clipped anyway as a backstop: the blend above is exact in real arithmetic, but rounding
        // can leave a channel a few ULPs outside, and callers convert straight to integers.
        return clip(desaturated);
    }

} // namespace SFT::Engine::Detail
