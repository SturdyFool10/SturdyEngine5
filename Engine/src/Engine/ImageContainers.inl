// Texture containers (DDS, KTX2), decoded to pixels so they go through the same paths as any other image. Included by
// ImageDecode.cpp inside its anonymous namespace: it shares that file's helpers and stb_image (for zlib).
//
// Only the base level of the first layer/face/slice is decoded; the engine builds its own mip chain. Block-compressed
// data is expanded to RGBA8 (BC1-5, BC7); half and full float data becomes Rgba16F in linear light. What is not handled
// (BC6H, ASTC, ETC2, Basis Universal, zstd supercompression) is reported as Unsupported rather than guessed at.

[[nodiscard]] bool looks_like_dds(std::span<const std::byte> encoded) noexcept {
    return encoded.size() >= 128 && std::memcmp(encoded.data(), "DDS ", 4) == 0;
}

[[nodiscard]] bool looks_like_ktx2(std::span<const std::byte> encoded) noexcept {
    static constexpr unsigned char magic[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    return encoded.size() >= 80 && std::memcmp(encoded.data(), magic, sizeof(magic)) == 0;
}

enum class ContainerCodec : u8 { Rgba8, Bgra8, R8, Rg8, Rgba16F, Rgba32F, Bc1, Bc2, Bc3, Bc4, Bc5, Bc7 };

[[nodiscard]] AssetExpected<DecodedImage> container_unsupported(const std::filesystem::path &source, const std::string &what) {
    return std::unexpected(AssetError{
        .code = AssetErrorCode::Unsupported,
        .message = UString{"'" + source.string() + "': " + what},
        .source = source,
    });
}

/// Bytes the base level of `codec` takes at `width` x `height`.
[[nodiscard]] usize container_level_bytes(ContainerCodec codec, u32 width, u32 height) noexcept {
    const usize blocks = static_cast<usize>((width + 3) / 4) * ((height + 3) / 4);
    switch (codec) {
    case ContainerCodec::Bc1:
    case ContainerCodec::Bc4: return blocks * 8;
    case ContainerCodec::Bc2:
    case ContainerCodec::Bc3:
    case ContainerCodec::Bc5:
    case ContainerCodec::Bc7: return blocks * 16;
    case ContainerCodec::R8: return static_cast<usize>(width) * height;
    case ContainerCodec::Rg8: return static_cast<usize>(width) * height * 2;
    case ContainerCodec::Rgba8:
    case ContainerCodec::Bgra8: return static_cast<usize>(width) * height * 4;
    case ContainerCodec::Rgba16F: return static_cast<usize>(width) * height * 8;
    case ContainerCodec::Rgba32F: return static_cast<usize>(width) * height * 16;
    }
    return 0;
}

/// Expands one level of `codec` data to the engine's decoded-image form.
[[nodiscard]] AssetExpected<DecodedImage> expand_container_level(ContainerCodec codec, u32 width, u32 height,
                                                                 std::span<const std::byte> data, bool srgb,
                                                                 const std::filesystem::path &source) {
    if (width == 0 || height == 0 || data.size() < container_level_bytes(codec, width, height)) {
        return container_unsupported(source, "the texture data is truncated.");
    }
    DecodedImage image;
    image.width = width;
    image.height = height;
    image.color_primaries = 1;
    const usize pixels = static_cast<usize>(width) * height;
    ImageFrame frame;

    if (codec == ContainerCodec::Rgba16F || codec == ContainerCodec::Rgba32F) {
        image.format = PixelFormat::Rgba16F;
        image.transfer_function = 8; // linear light
        frame.pixels.resize(pixels * 8);
        if (codec == ContainerCodec::Rgba16F) {
            std::memcpy(frame.pixels.data(), data.data(), frame.pixels.size());
        } else {
            auto *out = reinterpret_cast<u16 *>(frame.pixels.data());
            for (usize i = 0; i < pixels * 4; ++i) {
                f32 value;
                std::memcpy(&value, data.data() + i * 4, 4);
                out[i] = float_to_half(value);
            }
        }
        image.frames.push_back(std::move(frame));
        return image;
    }

    image.format = PixelFormat::Rgba8;
    // The bytes are passed through as stored, whatever the file calls them (normal maps and masks are UNORM, colour is
    // sRGB): the caller's colour-space choice on load decides how the GPU reads them, as for a PNG.
    image.transfer_function = 13;
    (void)srgb;
    frame.pixels.resize(pixels * 4);
    auto *out = reinterpret_cast<u8 *>(frame.pixels.data());
    const auto *in = reinterpret_cast<const u8 *>(data.data());
    switch (codec) {
    case ContainerCodec::Rgba8: std::memcpy(out, in, pixels * 4); break;
    case ContainerCodec::Bgra8:
        for (usize i = 0; i < pixels; ++i) {
            out[i * 4 + 0] = in[i * 4 + 2];
            out[i * 4 + 1] = in[i * 4 + 1];
            out[i * 4 + 2] = in[i * 4 + 0];
            out[i * 4 + 3] = in[i * 4 + 3];
        }
        break;
    case ContainerCodec::R8:
        for (usize i = 0; i < pixels; ++i) {
            out[i * 4 + 0] = in[i];
            out[i * 4 + 1] = 0;
            out[i * 4 + 2] = 0;
            out[i * 4 + 3] = 255;
        }
        break;
    case ContainerCodec::Rg8:
        for (usize i = 0; i < pixels; ++i) {
            out[i * 4 + 0] = in[i * 2];
            out[i * 4 + 1] = in[i * 2 + 1];
            out[i * 4 + 2] = 0;
            out[i * 4 + 3] = 255;
        }
        break;
    default: {
        const u32 blocks_x = (width + 3) / 4;
        const u32 blocks_y = (height + 3) / 4;
        const usize block_bytes = (codec == ContainerCodec::Bc1 || codec == ContainerCodec::Bc4) ? 8 : 16;
        for (u32 by = 0; by < blocks_y; ++by) {
            for (u32 bx = 0; bx < blocks_x; ++bx) {
                const u8 *block = in + (static_cast<usize>(by) * blocks_x + bx) * block_bytes;
                u8 tile[16][4];
                switch (codec) {
                case ContainerCodec::Bc1: rgbcx::unpack_bc1(block, tile); break;
                case ContainerCodec::Bc2: {
                    rgbcx::unpack_bc1(block + 8, tile, false);
                    for (int p = 0; p < 16; ++p) {
                        const u8 nibble = (block[p / 2] >> ((p & 1) * 4)) & 0xF;
                        tile[p][3] = static_cast<u8>(nibble * 17);
                    }
                    break;
                }
                case ContainerCodec::Bc3: rgbcx::unpack_bc3(block, tile); break;
                case ContainerCodec::Bc4: {
                    u8 red[16 * 4];
                    rgbcx::unpack_bc4(block, red, 4);
                    for (int p = 0; p < 16; ++p) {
                        tile[p][0] = red[p * 4];
                        tile[p][1] = 0;
                        tile[p][2] = 0;
                        tile[p][3] = 255;
                    }
                    break;
                }
                case ContainerCodec::Bc5: {
                    u8 rg[16][4] = {};
                    rgbcx::unpack_bc5(block, rg, 0, 1, 4);
                    for (int p = 0; p < 16; ++p) {
                        tile[p][0] = rg[p][0];
                        tile[p][1] = rg[p][1];
                        tile[p][2] = 0;
                        tile[p][3] = 255;
                    }
                    break;
                }
                default: {
                    bc7decomp::color_rgba decoded[16];
                    bc7decomp::unpack_bc7(block, decoded);
                    for (int p = 0; p < 16; ++p) {
                        tile[p][0] = decoded[p].r;
                        tile[p][1] = decoded[p].g;
                        tile[p][2] = decoded[p].b;
                        tile[p][3] = decoded[p].a;
                    }
                    break;
                }
                }
                for (u32 py = 0; py < 4; ++py) {
                    for (u32 px = 0; px < 4; ++px) {
                        const u32 x = bx * 4 + px;
                        const u32 y = by * 4 + py;
                        if (x < width && y < height) {
                            std::memcpy(out + (static_cast<usize>(y) * width + x) * 4, tile[py * 4 + px], 4);
                        }
                    }
                }
            }
        }
        break;
    }
    }
    image.frames.push_back(std::move(frame));
    return image;
}

/// Pulls the channel selected by `mask` out of `value` and scales it to 8 bits.
[[nodiscard]] u8 masked_channel(u32 value, u32 mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    u32 shift = 0;
    while (((mask >> shift) & 1u) == 0) {
        ++shift;
    }
    u32 bits = 0;
    while (((mask >> (shift + bits)) & 1u) != 0 && shift + bits < 32) {
        ++bits;
    }
    const u32 raw = (value & mask) >> shift;
    const u32 max = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1u;
    return static_cast<u8>((static_cast<u64>(raw) * 255u + max / 2) / max);
}

[[nodiscard]] AssetExpected<DecodedImage> decode_dds(std::span<const std::byte> encoded, const std::filesystem::path &source) {
    const auto u32_at = [&](usize at) { return read_u32_le(encoded, at); };
    if (u32_at(4) != 124) {
        return container_unsupported(source, "not a valid DDS header.");
    }
    const u32 height = u32_at(12);
    const u32 width = u32_at(16);
    const u32 pf_flags = u32_at(80);
    const u32 fourcc = u32_at(84);
    const u32 bit_count = u32_at(88);
    usize data_offset = 128;
    ContainerCodec codec = ContainerCodec::Rgba8;
    bool srgb = true;

    const auto four = [](const char (&s)[5]) {
        return static_cast<u32>(static_cast<u8>(s[0])) | (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
               (static_cast<u32>(static_cast<u8>(s[2])) << 16) | (static_cast<u32>(static_cast<u8>(s[3])) << 24);
    };

    if ((pf_flags & 0x4) != 0) { // DDPF_FOURCC
        if (fourcc == four("DX10")) {
            if (encoded.size() < 148) {
                return container_unsupported(source, "the DX10 DDS header is truncated.");
            }
            data_offset = 148;
            const u32 dxgi = u32_at(128);
            switch (dxgi) {
            case 71: codec = ContainerCodec::Bc1; srgb = false; break;
            case 72: codec = ContainerCodec::Bc1; break;
            case 74: codec = ContainerCodec::Bc2; srgb = false; break;
            case 75: codec = ContainerCodec::Bc2; break;
            case 77: codec = ContainerCodec::Bc3; srgb = false; break;
            case 78: codec = ContainerCodec::Bc3; break;
            case 80: codec = ContainerCodec::Bc4; srgb = false; break;
            case 83: codec = ContainerCodec::Bc5; srgb = false; break;
            case 98: codec = ContainerCodec::Bc7; srgb = false; break;
            case 99: codec = ContainerCodec::Bc7; break;
            case 28: codec = ContainerCodec::Rgba8; srgb = false; break;
            case 29: codec = ContainerCodec::Rgba8; break;
            case 87: codec = ContainerCodec::Bgra8; srgb = false; break;
            case 91: codec = ContainerCodec::Bgra8; break;
            case 61: codec = ContainerCodec::R8; srgb = false; break;
            case 49: codec = ContainerCodec::Rg8; srgb = false; break;
            case 10: codec = ContainerCodec::Rgba16F; break;
            case 2: codec = ContainerCodec::Rgba32F; break;
            case 95:
            case 96: return container_unsupported(source, "BC6H textures are not supported.");
            default: return container_unsupported(source, "unsupported DXGI format " + std::to_string(dxgi) + ".");
            }
        } else if (fourcc == four("DXT1")) {
            codec = ContainerCodec::Bc1;
        } else if (fourcc == four("DXT2") || fourcc == four("DXT3")) {
            codec = ContainerCodec::Bc2;
        } else if (fourcc == four("DXT4") || fourcc == four("DXT5")) {
            codec = ContainerCodec::Bc3;
        } else if (fourcc == four("ATI1") || fourcc == four("BC4U")) {
            codec = ContainerCodec::Bc4;
            srgb = false;
        } else if (fourcc == four("ATI2") || fourcc == four("BC5U")) {
            codec = ContainerCodec::Bc5;
            srgb = false;
        } else if (fourcc == 113) {
            codec = ContainerCodec::Rgba16F;
        } else if (fourcc == 116) {
            codec = ContainerCodec::Rgba32F;
        } else {
            return container_unsupported(source, "unsupported DDS FourCC " + std::to_string(fourcc) + ".");
        }
    } else if ((pf_flags & 0x40) != 0 && (bit_count == 32 || bit_count == 24 || bit_count == 16)) { // DDPF_RGB
        // Legacy masked RGB(A): converted to Rgba8 here since the layouts vary (565, 4444, 8888, 24-bit...).
        const u32 r = u32_at(92), g = u32_at(96), b = u32_at(100), a = (pf_flags & 0x1) != 0 ? u32_at(104) : 0;
        const usize bytes = bit_count / 8;
        if (width == 0 || height == 0 || encoded.size() < data_offset + static_cast<usize>(width) * height * bytes) {
            return container_unsupported(source, "the DDS pixel data is truncated.");
        }
        DecodedImage image;
        image.width = width;
        image.height = height;
        image.format = PixelFormat::Rgba8;
        ImageFrame frame;
        frame.pixels.resize(static_cast<usize>(width) * height * 4);
        for (usize i = 0; i < static_cast<usize>(width) * height; ++i) {
            u32 value = 0;
            for (usize k = 0; k < bytes; ++k) {
                value |= static_cast<u32>(static_cast<u8>(encoded[data_offset + i * bytes + k])) << (8 * k);
            }
            auto *px = reinterpret_cast<u8 *>(frame.pixels.data()) + i * 4;
            px[0] = masked_channel(value, r);
            px[1] = masked_channel(value, g);
            px[2] = masked_channel(value, b);
            px[3] = a != 0 ? masked_channel(value, a) : 255;
        }
        image.frames.push_back(std::move(frame));
        return image;
    } else if ((pf_flags & 0x20000) != 0 && bit_count == 8) { // DDPF_LUMINANCE
        codec = ContainerCodec::R8;
        srgb = false;
    } else {
        return container_unsupported(source, "unsupported DDS pixel format.");
    }
    return expand_container_level(codec, width, height, encoded.subspan(std::min(data_offset, encoded.size())), srgb, source);
}

[[nodiscard]] AssetExpected<DecodedImage> decode_ktx2(std::span<const std::byte> encoded, const std::filesystem::path &source) {
    const auto u32_at = [&](usize at) { return read_u32_le(encoded, at); };
    const auto u64_at = [&](usize at) { return static_cast<u64>(u32_at(at)) | (static_cast<u64>(u32_at(at + 4)) << 32); };
    const u32 vk_format = u32_at(12);
    const u32 width = u32_at(20);
    const u32 height = std::max<u32>(u32_at(24), 1u);
    const u32 level_count = std::max<u32>(u32_at(40), 1u);
    const u32 supercompression = u32_at(44);
    if (vk_format == 0) {
        return container_unsupported(source, "Basis Universal (UASTC/ETC1S) KTX2 files need a transcoder this build does not have.");
    }
    if (supercompression == 1 || supercompression == 2) {
        return container_unsupported(source, supercompression == 1 ? "BasisLZ supercompression is not supported."
                                                                   : "Zstandard supercompression is not supported.");
    }
    if (supercompression != 0 && supercompression != 3) {
        return container_unsupported(source, "unknown KTX2 supercompression scheme.");
    }
    if (encoded.size() < 80 + static_cast<usize>(level_count) * 24) {
        return container_unsupported(source, "the KTX2 level index is truncated.");
    }
    ContainerCodec codec;
    bool srgb = false;
    switch (vk_format) {
    case 37: codec = ContainerCodec::Rgba8; break;
    case 43: codec = ContainerCodec::Rgba8; srgb = true; break;
    case 44: codec = ContainerCodec::Bgra8; break;
    case 50: codec = ContainerCodec::Bgra8; srgb = true; break;
    case 9: codec = ContainerCodec::R8; break;
    case 16: codec = ContainerCodec::Rg8; break;
    case 97: codec = ContainerCodec::Rgba16F; break;
    case 109: codec = ContainerCodec::Rgba32F; break;
    case 131: case 133: codec = ContainerCodec::Bc1; break;
    case 132: case 134: codec = ContainerCodec::Bc1; srgb = true; break;
    case 135: codec = ContainerCodec::Bc2; break;
    case 136: codec = ContainerCodec::Bc2; srgb = true; break;
    case 137: codec = ContainerCodec::Bc3; break;
    case 138: codec = ContainerCodec::Bc3; srgb = true; break;
    case 139: codec = ContainerCodec::Bc4; break;
    case 141: codec = ContainerCodec::Bc5; break;
    case 145: codec = ContainerCodec::Bc7; break;
    case 146: codec = ContainerCodec::Bc7; srgb = true; break;
    case 143:
    case 144: return container_unsupported(source, "BC6H textures are not supported.");
    default: return container_unsupported(source, "unsupported KTX2 vkFormat " + std::to_string(vk_format) + ".");
    }
    // Level 0 is the largest and comes first in the index; its offset is absolute.
    const u64 offset = u64_at(80);
    const u64 length = u64_at(88);
    if (offset > encoded.size() || length > encoded.size() - offset) {
        return container_unsupported(source, "the KTX2 level data is out of range.");
    }
    const auto level = encoded.subspan(static_cast<usize>(offset), static_cast<usize>(length));
    if (supercompression == 3) {
        int inflated_size = 0;
        char *inflated = stbi_zlib_decode_malloc(reinterpret_cast<const char *>(level.data()), static_cast<int>(level.size()),
                                                 &inflated_size);
        if (inflated == nullptr) {
            return container_unsupported(source, "the zlib-compressed KTX2 level could not be inflated.");
        }
        const std::unique_ptr<char, decltype(&std::free)> owner{inflated, &std::free};
        return expand_container_level(codec, width, height,
                                      std::span<const std::byte>{reinterpret_cast<const std::byte *>(inflated), static_cast<usize>(inflated_size)},
                                      srgb, source);
    }
    return expand_container_level(codec, width, height, level, srgb, source);
}
