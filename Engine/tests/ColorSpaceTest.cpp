#include <Engine/ColorSpace.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <optional>

namespace {

    using SFT::f32;
    using SFT::u8;
    using SFT::usize;
    namespace Detail = SFT::Engine::Detail;

    /// Reports whether two scalars agree to within `tolerance`.
    [[nodiscard]] bool close(f32 lhs, f32 rhs, f32 tolerance) {
        return std::abs(lhs - rhs) <= tolerance;
    }

    /// Reports whether every element of two matrices agrees to within `tolerance`.
    [[nodiscard]] bool close(const Detail::ColorMatrix3 &lhs, const Detail::ColorMatrix3 &rhs, f32 tolerance) {
        for (usize i = 0; i < 9; ++i) {
            if (!close(lhs.m[i], rhs.m[i], tolerance)) {
                return false;
            }
        }
        return true;
    }

    /// Reports whether every component of two colors agrees to within `tolerance`.
    [[nodiscard]] bool close(const std::array<f32, 3> &lhs, const std::array<f32, 3> &rhs, f32 tolerance) {
        return close(lhs[0], rhs[0], tolerance) && close(lhs[1], rhs[1], tolerance) &&
               close(lhs[2], rhs[2], tolerance);
    }

    /// BT.709 relative luminance, the quantity the `Desaturate` gamut mapping is built to preserve.
    [[nodiscard]] f32 luminance(const std::array<f32, 3> &rgb) {
        return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const Detail::ColorPrimaries srgb = Detail::working_space_primaries();
    const Detail::ColorPrimaries bt2020 = Detail::primaries_from_h273(9).value();
    const Detail::ColorPrimaries dci_p3 = Detail::primaries_from_h273(11).value();
    const Detail::ColorPrimaries display_p3 = Detail::primaries_from_h273(12).value();

    // --- Matrix primitives. ---
    {
        const Detail::ColorMatrix3 identity = Detail::ColorMatrix3::identity();
        const std::array<f32, 3> color{0.25f, 0.5f, 0.75f};
        assert(close(identity.apply(color), color, 0.0f));

        const Detail::ColorMatrix3 a{{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 10.0f}};
        assert(close(Detail::multiply(a, identity), a, 0.0f));
        const std::optional<Detail::ColorMatrix3> a_inverse = Detail::inverse(a);
        assert(a_inverse.has_value());
        assert(close(Detail::multiply(a, *a_inverse), identity, 1e-5f));

        // A singular matrix (its third row is the sum of the first two) must be reported, not
        // silently inverted into garbage.
        const Detail::ColorMatrix3 singular{{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 5.0f, 7.0f, 9.0f}};
        assert(!Detail::inverse(singular).has_value());
    }

    // --- H.273 primaries table. ---
    {
        assert(Detail::primaries_from_h273(1).has_value());  // BT.709.
        assert(Detail::primaries_from_h273(5).has_value());  // BT.470BG.
        assert(Detail::primaries_from_h273(9).has_value());  // BT.2020.
        assert(Detail::primaries_from_h273(11).has_value()); // DCI-P3.
        assert(Detail::primaries_from_h273(12).has_value()); // Display P3.
        assert(Detail::primaries_from_h273(22).has_value()); // EBU Tech 3213-E.
        // Codes that name no gamut: "unspecified", reserved, and ST 428's XYZ (see its comment).
        assert(!Detail::primaries_from_h273(0).has_value());
        assert(!Detail::primaries_from_h273(2).has_value());
        assert(!Detail::primaries_from_h273(3).has_value());
        assert(!Detail::primaries_from_h273(10).has_value());
        assert(!Detail::primaries_from_h273(255).has_value());

        // Code 1 must be exactly the working space, or every "already in the working space" fast
        // path in the decoder would be skipped for ordinary sRGB images.
        const Detail::ColorPrimaries code1 = Detail::primaries_from_h273(1).value();
        assert(code1.red.x == srgb.red.x && code1.red.y == srgb.red.y);
        assert(code1.white.x == srgb.white.x && code1.white.y == srgb.white.y);

        // Display P3 is DCI-P3's gamut on D65; that difference is exactly what chromatic
        // adaptation exists to handle, so the two must not be conflated.
        assert(display_p3.red.x == dci_p3.red.x && display_p3.green.y == dci_p3.green.y);
        assert(display_p3.white.x != dci_p3.white.x);
    }

    // --- RGB <-> XYZ derivation, against the published sRGB matrix. ---
    {
        const Detail::ColorMatrix3 to_xyz = Detail::rgb_to_xyz_matrix(srgb);
        // The canonical sRGB D65 matrix. The tolerance is 3e-4 rather than tighter because
        // published tables are derived from a more precise D65 (0.312727, 0.329023) than the
        // (0.3127, 0.3290) every one of these standards actually prints and this code therefore
        // uses; the disagreement is largest in the blue/Z term, at ~2.3e-4.
        const Detail::ColorMatrix3 published{{
            0.4124564f, 0.3575761f, 0.1804375f,
            0.2126729f, 0.7151522f, 0.0721750f,
            0.0193339f, 0.1191920f, 0.9503041f,
        }};
        assert(close(to_xyz, published, 3e-4f));

        // The middle row of RGB->XYZ *is* the luminance weight vector, so this pins the 0.2126 /
        // 0.7152 / 0.0722 constants used for luminance elsewhere in the engine to the same
        // derivation rather than letting them drift as independent magic numbers.
        assert(close(to_xyz.m[3], 0.2126f, 1e-3f));
        assert(close(to_xyz.m[4], 0.7152f, 1e-3f));
        assert(close(to_xyz.m[5], 0.0722f, 1e-3f));

        // RGB (1,1,1) must land exactly on the white point's tristimulus, with Y == 1. This is the
        // normalization the whole derivation is built around.
        const std::array<f32, 3> white = to_xyz.apply({1.0f, 1.0f, 1.0f});
        assert(close(white[1], 1.0f, 1e-5f));
        assert(close(white[0], srgb.white.x / srgb.white.y, 1e-5f));

        // XYZ->RGB must invert RGB->XYZ.
        const Detail::ColorMatrix3 from_xyz = Detail::xyz_to_rgb_matrix(srgb);
        assert(close(Detail::multiply(to_xyz, from_xyz), Detail::ColorMatrix3::identity(), 1e-5f));

        // ...and must agree with the constants Shaders/sturdy_spectral_common.slang hardcodes in
        // xyzToLinearSrgb, so the CPU and GPU halves of the pipeline cannot disagree.
        assert(close(from_xyz.m[0], 3.2404542f, 1e-3f));
        assert(close(from_xyz.m[1], -1.5371385f, 1e-3f));
        assert(close(from_xyz.m[4], 1.8760108f, 1e-3f));
        assert(close(from_xyz.m[8], 1.0572252f, 1e-3f));

        // Every standard gamut must derive a well-formed, invertible matrix.
        for (u8 code : {u8{1}, u8{4}, u8{5}, u8{6}, u8{7}, u8{8}, u8{9}, u8{11}, u8{12}, u8{22}}) {
            const Detail::ColorPrimaries primaries = Detail::primaries_from_h273(code).value();
            const Detail::ColorMatrix3 matrix = Detail::rgb_to_xyz_matrix(primaries);
            assert(Detail::inverse(matrix).has_value());
            assert(close(matrix.apply({1.0f, 1.0f, 1.0f})[1], 1.0f, 1e-5f));
        }
    }

    // --- Chromatic adaptation. ---
    {
        // Adapting a white point to itself is a no-op.
        assert(close(Detail::chromatic_adaptation_matrix(srgb.white, srgb.white),
                     Detail::ColorMatrix3::identity(), 1e-5f));

        // Adapting between genuinely different white points must not be: if this were identity,
        // DCI-P3 content would carry a visible color cast into the working space.
        const Detail::ColorMatrix3 dci_to_d65 =
            Detail::chromatic_adaptation_matrix(dci_p3.white, srgb.white);
        assert(!close(dci_to_d65, Detail::ColorMatrix3::identity(), 1e-3f));

        // Adaptation must take the source white exactly to the destination white -- that is its
        // defining property.
        const std::array<f32, 3> dci_white_xyz{dci_p3.white.x / dci_p3.white.y, 1.0f,
                                               (1.0f - dci_p3.white.x - dci_p3.white.y) / dci_p3.white.y};
        const std::array<f32, 3> adapted = dci_to_d65.apply(dci_white_xyz);
        assert(close(adapted[0], srgb.white.x / srgb.white.y, 1e-4f));
        assert(close(adapted[1], 1.0f, 1e-4f));

        // ...and must be reversible.
        const Detail::ColorMatrix3 d65_to_dci =
            Detail::chromatic_adaptation_matrix(srgb.white, dci_p3.white);
        assert(close(Detail::multiply(dci_to_d65, d65_to_dci), Detail::ColorMatrix3::identity(), 1e-4f));
    }

    // --- End-to-end conversion matrices. ---
    {
        // Converting to the same space is the identity, which is what lets the decoder skip the
        // per-pixel loop for images already in the working space.
        assert(close(Detail::color_conversion_matrix(srgb, srgb), Detail::ColorMatrix3::identity(), 1e-5f));

        // The derived sRGB->BT.2020 matrix must match the one Shaders/sturdy_common.slang hardcodes
        // in linearSrgbToLinearBt2020, which the HDR presentation path applies on the way out.
        // The input transform built here and that output transform are then provably inverses of
        // each other rather than two independently-typed tables that might drift apart.
        const Detail::ColorMatrix3 srgb_to_2020 = Detail::color_conversion_matrix(srgb, bt2020);
        const Detail::ColorMatrix3 shader{{
            0.6274040f, 0.3292820f, 0.0433136f,
            0.0690970f, 0.9195400f, 0.0113612f,
            0.0163916f, 0.0880132f, 0.8955950f,
        }};
        assert(close(srgb_to_2020, shader, 1e-5f));

        // Round-tripping through a wider gamut and back must return where it started.
        const Detail::ColorMatrix3 back = Detail::color_conversion_matrix(bt2020, srgb);
        assert(close(Detail::multiply(back, srgb_to_2020), Detail::ColorMatrix3::identity(), 1e-4f));

        // White must survive every conversion as white -- including from DCI-P3, whose white point
        // is not D65 and only lands correctly because of the Bradford adaptation above.
        for (const Detail::ColorPrimaries &source : {bt2020, dci_p3, display_p3}) {
            const std::array<f32, 3> white =
                Detail::color_conversion_matrix(source, srgb).apply({1.0f, 1.0f, 1.0f});
            assert(close(white, {1.0f, 1.0f, 1.0f}, 1e-3f));
        }

        // A saturated wide-gamut primary has no in-gamut sRGB coordinate, so conversion must
        // produce out-of-range components rather than silently plausible ones. This is what the
        // gamut mapping below exists to clean up.
        const std::array<f32, 3> wide_green = back.apply({0.0f, 1.0f, 0.0f});
        assert(wide_green[0] < 0.0f);
        assert(wide_green[2] < 0.0f);
        assert(wide_green[1] > 1.0f);
    }

    // --- Gamut mapping. ---
    {
        const Detail::ColorMatrix3 bt2020_to_srgb = Detail::color_conversion_matrix(bt2020, srgb);
        const std::array<f32, 3> wide_green = bt2020_to_srgb.apply({0.0f, 1.0f, 0.0f});

        // An in-gamut color must pass through untouched under either policy.
        const std::array<f32, 3> in_gamut{0.25f, 0.5f, 0.75f};
        assert(close(Detail::map_into_gamut(in_gamut, Detail::GamutMapping::Desaturate, 1.0f), in_gamut, 0.0f));
        assert(close(Detail::map_into_gamut(in_gamut, Detail::GamutMapping::Clip, 1.0f), in_gamut, 0.0f));

        // Clip does exactly what it says, per channel.
        const std::array<f32, 3> clipped =
            Detail::map_into_gamut(wide_green, Detail::GamutMapping::Clip, 1.0f);
        assert(close(clipped, {0.0f, 1.0f, 0.0f}, 1e-5f));

        const std::array<f32, 3> desaturated =
            Detail::map_into_gamut(wide_green, Detail::GamutMapping::Desaturate, 1.0f);
        // In range...
        for (usize i = 0; i < 3; ++i) {
            assert(desaturated[i] >= 0.0f && desaturated[i] <= 1.0f);
        }
        // ...and, unlike clipping, it preserves the color's luminance exactly. Blending toward the
        // achromatic value of the same luminance cannot change luminance, which is the whole
        // reason this is the default.
        assert(close(luminance(desaturated), luminance(wide_green), 1e-3f));
        assert(!close(luminance(clipped), luminance(wide_green), 1e-3f));
        // It stays a green: the mapping desaturates rather than shifting hue.
        assert(desaturated[1] > desaturated[0] && desaturated[1] > desaturated[2]);

        // With no ceiling (an HDR float destination) only the negatives are removed; highlights
        // above 1.0 are the point of that destination and must survive.
        const f32 infinity = std::numeric_limits<f32>::infinity();
        const std::array<f32, 3> highlight{4.0f, 2.0f, 1.5f};
        assert(close(Detail::map_into_gamut(highlight, Detail::GamutMapping::Desaturate, infinity),
                     highlight, 0.0f));
        const std::array<f32, 3> hdr_mapped =
            Detail::map_into_gamut({-0.5f, 2.0f, 0.25f}, Detail::GamutMapping::Desaturate, infinity);
        assert(hdr_mapped[0] >= 0.0f);
        assert(hdr_mapped[1] > 1.0f); // Still an HDR highlight, not clamped to the SDR range.
    }

    return 0;
}
