// `.cube` parsing, the identity LUT, and the atlas layout the colour grade samples (Engine/ColorLut.cpp). All CPU.

#include <Engine/ColorLut.hpp>

#include <Engine/HdrTransfer.hpp>

#include <cmath>
#include <cstdio>

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
} // namespace

int main() {
    using namespace SFT::Engine;

    // Identity: every entry is its own normalised coordinate, red fastest.
    {
        const ColorLut identity = make_identity_color_lut(4);
        check(identity.valid() && identity.size == 4 && identity.rgb.size() == 4 * 4 * 4 * 3, "a 4^3 identity LUT has 64 triples");
        // Entry (r=1, g=2, b=3) -> index ((3 * 4 + 2) * 4 + 1) * 3.
        const size_t i = ((3 * 4 + 2) * 4 + 1) * 3;
        check(near(identity.rgb[i], 1.0f / 3.0f) && near(identity.rgb[i + 1], 2.0f / 3.0f) && near(identity.rgb[i + 2], 1.0f), "entry (1, 2, 3) is (1/3, 2/3, 1)");
        check(make_identity_color_lut(1).size == 2 && make_identity_color_lut(9999).size == 256, "sizes are clamped to 2..256");
    }

    // A 2x2x2 .cube: red fastest. Comments, TITLE and DOMAIN lines are skipped.
    {
        const char *text =
            "# a comment\n"
            "TITLE \"test\"\n"
            "DOMAIN_MIN 0 0 0\n"
            "DOMAIN_MAX 1 1 1\n"
            "LUT_3D_SIZE 2\n"
            "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\r\n";
        const auto lut = parse_cube_lut(text);
        check(lut.has_value() && lut->valid() && lut->size == 2, "a valid 2^3 cube parses");
        if (lut) {
            const ColorLut identity = make_identity_color_lut(2);
            check(lut->rgb == identity.rgb, "the standard 2^3 cube equals the identity LUT (red fastest, CRLF tolerated)");
        }
    }

    // Errors carry a message and never produce a half-built LUT.
    {
        check(!parse_cube_lut("0 0 0\n").has_value(), "data before LUT_3D_SIZE is rejected");
        check(!parse_cube_lut("LUT_1D_SIZE 16\n").has_value(), "1D LUTs are rejected");
        check(!parse_cube_lut("LUT_3D_SIZE 1\n").has_value(), "a size below 2 is rejected");
        check(!parse_cube_lut("LUT_3D_SIZE 2\n0 0 0\n").has_value(), "too few values are rejected");
        check(!parse_cube_lut("LUT_3D_SIZE 2\n0 0\n").has_value(), "a short data line is rejected");
        check(!parse_cube_lut("").has_value(), "an empty file is rejected");
        std::string too_many = "LUT_3D_SIZE 2\n";
        for (int i = 0; i < 9; ++i) too_many += "0 0 0\n";
        check(!parse_cube_lut(too_many).has_value(), "too many values are rejected");
        check(!load_cube_lut("/nonexistent/path.cube").has_value(), "a missing file is an error, not a crash");
    }

    // Atlas layout: (N * N) wide, N tall, slice b at columns [b * N, (b + 1) * N), red along x, green along y, RGBA16F.
    {
        ColorLut lut = make_identity_color_lut(3);
        // Mark one entry (r=2, g=1, b=0) with a distinctive colour.
        const size_t i = ((0 * 3 + 1) * 3 + 2) * 3;
        lut.rgb[i] = 0.25f;
        lut.rgb[i + 1] = 0.5f;
        lut.rgb[i + 2] = 0.75f;
        const std::vector<SFT::u16> atlas = color_lut_to_half_atlas(lut);
        check(atlas.size() == 9 * 3 * 4, "the atlas is 9 x 3 RGBA texels");
        // Texel for (r=2, g=1, b=0): x = 0 * 3 + 2 = 2, y = 1.
        const size_t texel = 1 * 9 + 2;
        check(near(Detail::half_to_float(atlas[texel * 4 + 0]), 0.25f) && near(Detail::half_to_float(atlas[texel * 4 + 1]), 0.5f) &&
                  near(Detail::half_to_float(atlas[texel * 4 + 2]), 0.75f) && near(Detail::half_to_float(atlas[texel * 4 + 3]), 1.0f),
              "entry (r=2, g=1, b=0) lands at atlas column 2, row 1 with alpha 1");
        // Blue selects the slice: the identity entry (r=1, g=0, b=2) sits at column 2 * 3 + 1 = 7, row 0.
        const ColorLut identity = make_identity_color_lut(3);
        const std::vector<SFT::u16> plain = color_lut_to_half_atlas(identity);
        const size_t slice_texel = 0 * 9 + (2 * 3 + 1);
        check(near(Detail::half_to_float(plain[slice_texel * 4 + 0]), 0.5f) && near(Detail::half_to_float(plain[slice_texel * 4 + 1]), 0.0f) &&
                  near(Detail::half_to_float(plain[slice_texel * 4 + 2]), 1.0f),
              "blue selects the slice (left to right)");
        check(color_lut_to_half_atlas(ColorLut{}).empty(), "an invalid LUT gives an empty atlas");
    }

    if (failures == 0) std::printf("ColorLutTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
