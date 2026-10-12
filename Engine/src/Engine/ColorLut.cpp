#include <Engine/ColorLut.hpp>

#include <charconv>
#include <cstring>

#include <Engine/HdrTransfer.hpp>

namespace SFT::Engine {

    namespace {

        constexpr u32 kMaxLutSize = 256;

        [[nodiscard]] std::string_view trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
            return text;
        }

        [[nodiscard]] bool starts_with_keyword(std::string_view line, std::string_view keyword) {
            return line.size() > keyword.size() && line.substr(0, keyword.size()) == keyword &&
                   (line[keyword.size()] == ' ' || line[keyword.size()] == '\t');
        }

        /// Parses up to `count` whitespace-separated floats from `text`; returns how many were read.
        [[nodiscard]] usize parse_floats(std::string_view text, f32 *out, usize count) {
            usize read = 0;
            while (read < count) {
                while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == ',')) text.remove_prefix(1);
                if (text.empty()) break;
                f32 value = 0.0f;
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error != std::errc{}) break;
                out[read++] = value;
                text.remove_prefix(static_cast<usize>(end - text.data()));
            }
            return read;
        }

    } // namespace

    ColorLut make_identity_color_lut(u32 size) {
        ColorLut lut;
        lut.size = std::clamp(size, 2u, kMaxLutSize);
        const f32 scale = 1.0f / static_cast<f32>(lut.size - 1);
        lut.rgb.reserve(static_cast<usize>(lut.size) * lut.size * lut.size * 3u);
        for (u32 b = 0; b < lut.size; ++b) {
            for (u32 g = 0; g < lut.size; ++g) {
                for (u32 r = 0; r < lut.size; ++r) {
                    lut.rgb.push_back(static_cast<f32>(r) * scale);
                    lut.rgb.push_back(static_cast<f32>(g) * scale);
                    lut.rgb.push_back(static_cast<f32>(b) * scale);
                }
            }
        }
        return lut;
    }

    std::expected<ColorLut, std::string> parse_cube_lut(std::string_view text) {
        ColorLut lut;
        usize entries = 0;
        usize line_number = 0;
        while (!text.empty()) {
            const usize newline = text.find('\n');
            std::string_view line = trim(text.substr(0, newline));
            text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
            ++line_number;
            if (line.empty() || line.front() == '#') continue;

            if (starts_with_keyword(line, "LUT_1D_SIZE")) {
                return std::unexpected("1D LUTs are not supported (line " + std::to_string(line_number) + ")");
            }
            if (starts_with_keyword(line, "LUT_3D_SIZE")) {
                u32 size = 0;
                const std::string_view number = trim(line.substr(11));
                const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), size);
                if (error != std::errc{} || size < 2 || size > kMaxLutSize) {
                    return std::unexpected("LUT_3D_SIZE must be a number from 2 to " + std::to_string(kMaxLutSize) + " (line " +
                                           std::to_string(line_number) + ")");
                }
                lut.size = size;
                lut.rgb.assign(static_cast<usize>(size) * size * size * 3u, 0.0f);
                continue;
            }
            if (starts_with_keyword(line, "TITLE") || starts_with_keyword(line, "DOMAIN_MIN") || starts_with_keyword(line, "DOMAIN_MAX") ||
                starts_with_keyword(line, "LUT_3D_INPUT_RANGE") || starts_with_keyword(line, "LUT_IN_VIDEO_RANGE") ||
                starts_with_keyword(line, "LUT_OUT_VIDEO_RANGE")) {
                continue;
            }

            // A data line: three floats.
            if (lut.size == 0) {
                return std::unexpected("LUT data before LUT_3D_SIZE (line " + std::to_string(line_number) + ")");
            }
            f32 rgb[3];
            if (parse_floats(line, rgb, 3) != 3) {
                return std::unexpected("expected three numbers (line " + std::to_string(line_number) + ")");
            }
            if (entries >= static_cast<usize>(lut.size) * lut.size * lut.size) {
                return std::unexpected("more values than LUT_3D_SIZE^3 (line " + std::to_string(line_number) + ")");
            }
            std::memcpy(&lut.rgb[entries * 3u], rgb, sizeof(rgb));
            ++entries;
        }
        if (lut.size == 0) {
            return std::unexpected("no LUT_3D_SIZE found");
        }
        if (entries != static_cast<usize>(lut.size) * lut.size * lut.size) {
            return std::unexpected("expected " + std::to_string(static_cast<usize>(lut.size) * lut.size * lut.size) + " values, found " +
                                   std::to_string(entries));
        }
        return lut;
    }

    std::expected<ColorLut, std::string> load_cube_lut(const std::string &path) {
        const std::optional<std::string> text = Foundation::read_file_to_string(path);
        if (!text) {
            return std::unexpected("could not read " + path);
        }
        auto lut = parse_cube_lut(*text);
        if (!lut) {
            return std::unexpected(path + ": " + lut.error());
        }
        return lut;
    }

    std::vector<u16> color_lut_to_half_atlas(const ColorLut &lut) {
        std::vector<u16> atlas;
        if (!lut.valid()) {
            return atlas;
        }
        const u32 n = lut.size;
        const u16 one = Detail::float_to_half(1.0f);
        atlas.assign(static_cast<usize>(n) * n * n * 4u, 0u);
        for (u32 b = 0; b < n; ++b) {
            for (u32 g = 0; g < n; ++g) {
                for (u32 r = 0; r < n; ++r) {
                    const usize source = ((static_cast<usize>(b) * n + g) * n + r) * 3u;
                    // Slice b occupies columns [b * n, (b + 1) * n); the row is green.
                    const usize texel = static_cast<usize>(g) * (static_cast<usize>(n) * n) + static_cast<usize>(b) * n + r;
                    atlas[texel * 4u + 0u] = Detail::float_to_half(lut.rgb[source + 0u]);
                    atlas[texel * 4u + 1u] = Detail::float_to_half(lut.rgb[source + 1u]);
                    atlas[texel * 4u + 2u] = Detail::float_to_half(lut.rgb[source + 2u]);
                    atlas[texel * 4u + 3u] = one;
                }
            }
        }
        return atlas;
    }

    Core::RendererExpected<Renderer::TextureHandle> create_color_lut(Renderer::Renderer &renderer, const ColorLut &lut) {
        if (!lut.valid()) {
            return std::unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              "create_color_lut: the LUT is not a valid size^3 table."});
        }
        const std::vector<u16> atlas = color_lut_to_half_atlas(lut);
        return renderer.create_texture(lut.size * lut.size, lut.size, RHI::Format::RGBA16Float,
                                       std::as_bytes(std::span<const u16>{atlas.data(), atlas.size()}), "colour grade LUT");
    }

} // namespace SFT::Engine
