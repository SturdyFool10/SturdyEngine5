// EXIF orientation is applied to decoded images the way a browser shows them. Built on tiny hand-made PNGs (stored
// zlib blocks) carrying an eXIf chunk, in both TIFF byte orders.
#include <Engine/ImageDecode.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    using Bytes = std::vector<std::byte>;

    void be32(Bytes &out, uint32_t v) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
    void text(Bytes &out, const char *t, size_t n) {
        for (size_t i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(t[i]));
    }

    uint32_t crc32(const std::byte *data, size_t size) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= static_cast<uint8_t>(data[i]);
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    void chunk(Bytes &png, const char *type, const Bytes &body) {
        be32(png, static_cast<uint32_t>(body.size()));
        const size_t start = png.size();
        text(png, type, 4);
        png.insert(png.end(), body.begin(), body.end());
        be32(png, crc32(png.data() + start, png.size() - start));
    }

    /// Two pixels side by side, red then blue, with `orientation` stored in an eXIf chunk.
    Bytes png_2x1(int orientation, bool little_endian) {
        Bytes png;
        text(png, "\x89PNG\r\n\x1a\n", 8);
        Bytes ihdr;
        be32(ihdr, 2); be32(ihdr, 1);
        for (uint8_t b : {8, 6, 0, 0, 0}) ihdr.push_back(static_cast<std::byte>(b)); // depth 8, RGBA
        chunk(png, "IHDR", ihdr);

        Bytes exif;
        const auto u16 = [&](uint16_t v) {
            if (little_endian) { exif.push_back(static_cast<std::byte>(v & 0xff)); exif.push_back(static_cast<std::byte>(v >> 8)); }
            else { exif.push_back(static_cast<std::byte>(v >> 8)); exif.push_back(static_cast<std::byte>(v & 0xff)); }
        };
        const auto u32 = [&](uint32_t v) { u16(little_endian ? v & 0xffff : v >> 16); u16(little_endian ? v >> 16 : v & 0xffff); };
        text(exif, little_endian ? "II" : "MM", 2);
        u16(42); u32(8); u16(1);          // header, IFD0 at 8, one entry
        u16(0x0112); u16(3); u32(1); u16(static_cast<uint16_t>(orientation)); u16(0); u32(0);
        chunk(png, "eXIf", exif);

        // Scanline: filter 0 + red + blue. Stored deflate block with the zlib header and adler32.
        const std::vector<uint8_t> raw = {0, 255, 0, 0, 255, 0, 0, 255, 255};
        Bytes z = {std::byte{0x78}, std::byte{0x01}, std::byte{0x01}};
        z.push_back(static_cast<std::byte>(raw.size())); z.push_back(std::byte{0});
        z.push_back(static_cast<std::byte>(~raw.size() & 0xff)); z.push_back(std::byte{0xff});
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw) { z.push_back(static_cast<std::byte>(v)); a = (a + v) % 65521; b = (b + a) % 65521; }
        be32(z, (b << 16) | a);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        return png;
    }

    bool pixel_is(const SFT::Engine::DecodedImage &image, int x, int y, uint8_t r, uint8_t b) {
        const auto *p = reinterpret_cast<const uint8_t *>(image.pixels().data()) + (static_cast<size_t>(y) * image.width + x) * 4;
        return p[0] == r && p[2] == b && p[3] == 255;
    }

} // namespace

int main() {
    using namespace SFT::Engine;
    const auto decode = [](const Bytes &png, bool apply) {
        DecodeOptions options;
        options.apply_orientation = apply;
        return decode_image(png, options, "orientation.png");
    };

    {
        auto image = decode(png_2x1(1, true), true);
        check(image && image->width == 2 && image->height == 1, "orientation 1 keeps the size");
        if (image) check(pixel_is(*image, 0, 0, 255, 0) && pixel_is(*image, 1, 0, 0, 255), "orientation 1 keeps the pixels");
    }
    {
        auto image = decode(png_2x1(6, true), true); // rotate clockwise: red on top
        check(image && image->width == 1 && image->height == 2, "orientation 6 swaps the size");
        if (image) check(pixel_is(*image, 0, 0, 255, 0) && pixel_is(*image, 0, 1, 0, 255), "orientation 6 pixels");
    }
    {
        auto image = decode(png_2x1(8, false), true); // rotate counter-clockwise (big-endian EXIF): blue on top
        check(image && image->width == 1 && image->height == 2, "orientation 8 swaps the size");
        if (image) check(pixel_is(*image, 0, 0, 0, 255) && pixel_is(*image, 0, 1, 255, 0), "orientation 8 pixels");
    }
    {
        auto image = decode(png_2x1(2, true), true); // mirror
        if (image) check(pixel_is(*image, 0, 0, 0, 255) && pixel_is(*image, 1, 0, 255, 0), "orientation 2 mirrors");
    }
    {
        auto image = decode(png_2x1(3, false), true); // 180 degrees
        if (image) check(pixel_is(*image, 0, 0, 0, 255) && pixel_is(*image, 1, 0, 255, 0), "orientation 3 turns 180");
    }
    {
        auto image = decode(png_2x1(6, true), false);
        check(image && image->width == 2 && image->height == 1, "apply_orientation=false leaves the stored pixels");
    }
    if (failures == 0) std::cout << "OK\n";
    return failures == 0 ? 0 : 1;
}
