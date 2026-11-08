// DDS and KTX2 textures decode to pixels: uncompressed, block-compressed, legacy masked RGB, zlib supercompression, and
// the formats that are refused (BC6H, Basis, zstd) say so.
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
    void le32(Bytes &out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
    void le64(Bytes &out, uint64_t v) { le32(out, static_cast<uint32_t>(v)); le32(out, static_cast<uint32_t>(v >> 32)); }
    void raw(Bytes &out, std::initializer_list<uint8_t> v) { for (uint8_t b : v) out.push_back(static_cast<std::byte>(b)); }
    void four(Bytes &out, const char *t) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(t[i])); }

    /// A DDS header. `fourcc` null means a masked-RGB pixel format of `bits` bits.
    Bytes dds(uint32_t w, uint32_t h, const char *fourcc, uint32_t dxgi, uint32_t bits, const Bytes &data) {
        Bytes out;
        four(out, "DDS ");
        le32(out, 124); le32(out, 0x1007); le32(out, h); le32(out, w); le32(out, 0); le32(out, 0); le32(out, 1);
        for (int i = 0; i < 11; ++i) le32(out, 0);
        le32(out, 32);
        if (fourcc) { le32(out, 4); four(out, fourcc); for (int i = 0; i < 5; ++i) le32(out, 0); }
        else { le32(out, 0x41); le32(out, 0); le32(out, bits); le32(out, 0x000000FF); le32(out, 0x0000FF00); le32(out, 0x00FF0000); le32(out, 0xFF000000); }
        le32(out, 0x1000); for (int i = 0; i < 4; ++i) le32(out, 0);
        if (fourcc && std::memcmp(fourcc, "DX10", 4) == 0) { le32(out, dxgi); le32(out, 3); le32(out, 0); le32(out, 1); le32(out, 0); }
        out.insert(out.end(), data.begin(), data.end());
        return out;
    }

    Bytes ktx2(uint32_t vk, uint32_t w, uint32_t h, uint32_t scheme, const Bytes &level) {
        Bytes out;
        raw(out, {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'});
        le32(out, vk); le32(out, 1); le32(out, w); le32(out, h); le32(out, 0); le32(out, 0); le32(out, 1); le32(out, 1); le32(out, scheme);
        le32(out, 0); le32(out, 0); le32(out, 0); le32(out, 0); le64(out, 0); le64(out, 0); // dfd, kvd, sgd
        le64(out, 104); le64(out, level.size()); le64(out, level.size());                    // one level
        while (out.size() < 104) out.push_back(std::byte{0});
        out.insert(out.end(), level.begin(), level.end());
        return out;
    }

    const uint8_t *px(const SFT::Engine::DecodedImage &image, int x, int y) {
        return reinterpret_cast<const uint8_t *>(image.pixels().data()) + (static_cast<size_t>(y) * image.width + x) * 4;
    }

} // namespace

int main() {
    using namespace SFT::Engine;
    const auto decode = [](const Bytes &bytes, const char *name) { return decode_image(bytes, DecodeOptions{}, name); };

    {   // BC1 block: both endpoints red-then-blue, every index 0 -> solid red
        Bytes block; raw(block, {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
        auto image = decode(dds(4, 4, "DXT1", 0, 0, block), "a.dds");
        check(image && image->width == 4 && image->height == 4, "DXT1 size");
        if (image) check(px(*image, 2, 2)[0] >= 248 && px(*image, 2, 2)[2] <= 8 && px(*image, 3, 3)[3] == 255, "DXT1 colour");
    }
    {   // BC3: alpha endpoints 0 and 255, index 0 -> alpha 0; colour solid red
        Bytes block; raw(block, {0, 255, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
        auto image = decode(dds(4, 4, "DXT5", 0, 0, block), "b.dds");
        if (image) check(px(*image, 0, 0)[3] == 0 && px(*image, 0, 0)[0] >= 248, "DXT5 colour and alpha");
        else check(false, "DXT5 decodes");
    }
    {   // BC2: explicit alpha nibble 0xF everywhere
        Bytes block(8, std::byte{0xFF}); raw(block, {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
        auto image = decode(dds(4, 4, "DXT3", 0, 0, block), "c.dds");
        if (image) check(px(*image, 1, 1)[3] == 255 && px(*image, 1, 1)[0] >= 248, "DXT3");
        else check(false, "DXT3 decodes");
    }
    {   // BC4 (R only) via the DX10 header
        Bytes block; raw(block, {200, 200, 0, 0, 0, 0, 0, 0});
        auto image = decode(dds(4, 4, "DX10", 80, 0, block), "d.dds");
        if (image) check(px(*image, 0, 0)[0] == 200 && px(*image, 0, 0)[3] == 255, "BC4");
        else check(false, "BC4 decodes");
    }
    {   // BC7 decodes without error (content is whatever the block says)
        Bytes block(16, std::byte{0x40});
        check(decode(dds(4, 4, "DX10", 98, 0, block), "e.dds").has_value(), "BC7 decodes");
    }
    {   // Legacy 32-bit masked RGBA, 2x1
        Bytes data; raw(data, {255, 0, 0, 255, 0, 0, 255, 128});
        auto image = decode(dds(2, 1, nullptr, 0, 32, data), "f.dds");
        if (image) check(px(*image, 0, 0)[0] == 255 && px(*image, 1, 0)[2] == 255 && px(*image, 1, 0)[3] == 128, "masked RGBA8");
        else check(false, "masked RGBA8 decodes");
    }
    {   // Cropping: 5x5 BC1 -> 2x2 blocks, size stays 5x5
        Bytes blocks; for (int i = 0; i < 4; ++i) raw(blocks, {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0});
        auto image = decode(dds(5, 5, "DXT1", 0, 0, blocks), "g.dds");
        check(image && image->width == 5 && image->height == 5 && image->pixels().size() == 5 * 5 * 4, "partial blocks cropped");
    }
    {   // KTX2 RGBA8 sRGB, 2x1
        Bytes data; raw(data, {255, 0, 0, 255, 0, 255, 0, 255});
        auto image = decode(ktx2(43, 2, 1, 0, data), "a.ktx2");
        if (image) check(px(*image, 0, 0)[0] == 255 && px(*image, 1, 0)[1] == 255, "KTX2 RGBA8");
        else check(false, "KTX2 RGBA8 decodes");
    }
    {   // KTX2 with zlib supercompression (a stored deflate block)
        const std::vector<uint8_t> pixels = {255, 0, 0, 255, 0, 255, 0, 255};
        Bytes z; raw(z, {0x78, 0x01, 0x01, static_cast<uint8_t>(pixels.size()), 0, static_cast<uint8_t>(~pixels.size() & 0xff), 0xff});
        uint32_t a = 1, b = 0;
        for (uint8_t v : pixels) { z.push_back(static_cast<std::byte>(v)); a = (a + v) % 65521; b = (b + a) % 65521; }
        raw(z, {static_cast<uint8_t>(b >> 8), static_cast<uint8_t>(b), static_cast<uint8_t>(a >> 8), static_cast<uint8_t>(a)});
        auto image = decode(ktx2(37, 2, 1, 3, z), "z.ktx2");
        if (image) check(px(*image, 1, 0)[1] == 255, "KTX2 zlib");
        else check(false, "KTX2 zlib decodes");
    }
    {   // Refusals are explicit
        Bytes block(16, std::byte{0});
        auto bc6 = decode(dds(4, 4, "DX10", 95, 0, block), "h.dds");
        check(!bc6 && bc6.error().code == SFT::Engine::AssetErrorCode::Unsupported, "BC6H refused");
        auto basis = decode(ktx2(0, 4, 4, 1, block), "basis.ktx2");
        check(!basis && basis.error().code == SFT::Engine::AssetErrorCode::Unsupported, "Basis refused");
        auto zstd = decode(ktx2(37, 1, 1, 2, block), "zstd.ktx2");
        check(!zstd && zstd.error().code == SFT::Engine::AssetErrorCode::Unsupported, "zstd refused");
        check(!decode(dds(4, 4, "DXT1", 0, 0, Bytes(2)), "t.dds").has_value(), "truncated data refused");
    }
    if (failures == 0) std::cout << "OK\n";
    return failures == 0 ? 0 : 1;
}
