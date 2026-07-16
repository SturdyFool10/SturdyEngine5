// Throwaway diagnostic: rasterizes one glyph in isolation (Font -> hb-draw outline ->
// Text::rasterize_glyph) and dumps the raw SDF bitmap as a PGM, bypassing TextAtlas packing and
// the text_sdf.slang shader entirely. Lets us tell whether a suspected Y-axis bug lives in
// Text/Outline.cpp + Text/Raster.cpp (this program would show it directly) or further downstream
// in atlas packing / the shader (this program would look correct).
#include <Foundation/Foundation.hpp>

#include <Text/Text.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string_view>
#include <vector>

using namespace SFT;

namespace {

    [[nodiscard]] std::optional<std::vector<std::byte>> read_file_bytes(const char *path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return std::nullopt;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            return std::nullopt;
        }
        file.seekg(0);
        std::vector<std::byte> bytes(static_cast<usize>(size));
        if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
            return std::nullopt;
        }
        return bytes;
    }

} // namespace

int main(int argc, char **argv) {
    int arg_index = 1;
    f32 hinted_pixel_size = 0.0f;
    if (argc > 2 && std::string_view(argv[1]) == "--hinted") {
        hinted_pixel_size = static_cast<f32>(std::atof(argv[2]));
        arg_index = 3;
    }
    if (argc <= arg_index) {
        std::fprintf(stderr, "usage: %s [--hinted <pixel-size>] <font-path> [char] [out.pgm]\n", argv[0]);
        return 1;
    }
    const char *font_path = argv[arg_index];
    const char *selector = argc > arg_index + 1 ? argv[arg_index + 1] : "e";
    const char *out_path = argc > arg_index + 2 ? argv[arg_index + 2] : "/tmp/glyph_diag.pgm";

    std::optional<std::vector<std::byte>> bytes = read_file_bytes(font_path);
    if (!bytes) {
        std::fprintf(stderr, "failed to read font file: %s\n", font_path);
        return 1;
    }

    Text::TextExpected<Text::Font> font =
        hinted_pixel_size > 0.0f
            ? Text::Font::load_hinted(std::span<const std::byte>{bytes->data(), bytes->size()}, hinted_pixel_size)
            : Text::Font::load(std::span<const std::byte>{bytes->data(), bytes->size()});
    if (!font) {
        std::fprintf(stderr, "failed to load font\n");
        return 1;
    }

    hb_codepoint_t glyph_id = 0;
    if (selector[0] == '#') {
        glyph_id = static_cast<hb_codepoint_t>(std::atoi(selector + 1));
        std::printf("raw glyph_id %u, units_per_em=%u\n", glyph_id, font->units_per_em());
    } else {
        const u32 codepoint = static_cast<u32>(selector[0]);
        if (!hb_font_get_nominal_glyph(font->handle(), codepoint, &glyph_id)) {
            std::fprintf(stderr, "no glyph for codepoint U+%04X\n", codepoint);
            return 1;
        }
        std::printf("codepoint U+%04X -> glyph_id %u, units_per_em=%u\n", codepoint, glyph_id, font->units_per_em());
    }

    Text::TextExpected<Text::GlyphOutline> outline = Text::glyph_outline(*font, glyph_id);
    if (!outline) {
        std::fprintf(stderr, "failed to extract outline\n");
        return 1;
    }
    std::printf("contours: %zu\n", outline->contours.size());
    for (usize i = 0; i < outline->contours.size(); ++i) {
        const Text::Contour &contour = outline->contours[i];
        f32 min_y = 1e9f;
        f32 max_y = -1e9f;
        for (const Text::OutlineSegment &segment : contour) {
            min_y = std::min(min_y, segment.to.y);
            max_y = std::max(max_y, segment.to.y);
        }
        std::printf("  contour %zu: %zu segments, y range [%.1f, %.1f]\n", i, contour.size(), min_y, max_y);
    }

    constexpr u32 size = 256;
    const Text::RasterParams params{
        .width = size,
        .height = size,
        .scale = static_cast<f32>(size - 40) / static_cast<f32>(font->units_per_em()),
        .pixel_range = 8.0f,
        .padding_px = 20.0f,
    };
    Text::TextExpected<Text::RasterizedGlyph> raster = Text::rasterize_glyph(*outline, Text::RasterFormat::SDF, params);
    if (!raster) {
        std::fprintf(stderr, "failed to rasterize glyph\n");
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary);
    out << "P5\n" << raster->width << " " << raster->height << "\n255\n";
    out.write(reinterpret_cast<const char *>(raster->pixels.data()), static_cast<std::streamsize>(raster->pixels.size()));
    std::printf("wrote %s (%ux%u)\n", out_path, raster->width, raster->height);
    return 0;
}
