#include <Engine/AssetManager.hpp>
#include <Engine/TextureCompression.hpp>
#include <Engine/TextureMipChain.hpp>

#include <Core/src/Core/Decompression.hpp>

#include <RHI/RHI.hpp>

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

int main() {
    using namespace SFT;

    // A pattern with real structure (not random noise, which GDeflate/DEFLATE-family codecs can't
    // meaningfully compress) so the round trip below exercises actual compression, not just a
    // pass-through -- repeating byte runs mimic a real BC7-block stream's redundancy far better than
    // uniform random bytes would.
    std::vector<std::byte> source(64 * 1024);
    for (usize i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::byte>((i / 37) % 251);
    }

    // Mip generation includes the complete chain and filters sRGB colors in linear light.
    {
        const std::vector<std::byte> checker{
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255},
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255},
            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
        };
        auto srgb_mips = Engine::Detail::generate_rgba8_mip_chain(checker, 2, 2, true);
        assert(srgb_mips.has_value());
        assert(srgb_mips->mip_levels == 2);
        assert(srgb_mips->data.size() == checker.size() + 4);
        const u8 filtered = std::to_integer<u8>(srgb_mips->data[checker.size()]);
        assert(filtered >= 187 && filtered <= 188);
        assert(std::to_integer<u8>(srgb_mips->data.back()) == 255);

        auto linear_mips = Engine::Detail::generate_rgba8_mip_chain(checker, 2, 2, false);
        assert(linear_mips.has_value());
        assert(std::to_integer<u8>(linear_mips->data[checker.size()]) == 128);
        assert(Engine::Detail::texture_mip_level_count(3, 5) == 3);

        std::vector<std::byte> non_power_of_two(5 * 4, std::byte{0});
        for (usize pixel = 0; pixel < 5; ++pixel) {
            non_power_of_two[pixel * 4 + 3] = std::byte{255};
        }
        non_power_of_two[2 * 4] = std::byte{255};
        auto npot_mips = Engine::Detail::generate_rgba8_mip_chain(non_power_of_two, 5, 1, false);
        assert(npot_mips.has_value());
        assert(npot_mips->mip_levels == 3);
        assert(npot_mips->data.size() == 32);
        assert(std::to_integer<u8>(npot_mips->data[non_power_of_two.size() + 2 * 4]) == 51);
        assert(std::to_integer<u8>(npot_mips->data.back()) == 255);
    }

    // Every mip, including sub-4x4 tail levels, remains BC7-compressed and tightly packed.
    {
        std::vector<std::byte> rgba(4 * 4 * 4, std::byte{127});
        auto mips = Engine::Detail::generate_rgba8_mip_chain(rgba, 4, 4, false);
        assert(mips.has_value());
        auto bc7 = Engine::Detail::compress_bc7_mip_chain(mips->data, 4, 4, mips->mip_levels, false);
        assert(bc7.has_value());
        assert(bc7->size() == 3 * 16);
    }

    // Core::compress_gdeflate / Core::decompress_gdeflate direct round trip.
    {
        auto compressed = Core::compress_gdeflate(std::span<const std::byte>{source});
        assert(compressed.has_value());
        assert(!compressed->empty());

        auto decompressed = Core::decompress_gdeflate(std::span<const std::byte>{*compressed}, source.size());
        assert(decompressed.has_value());
        assert(decompressed->size() == source.size());
        assert(*decompressed == source);
    }

    // BC1/BC3/BC4/BC5 encoders (rgbcx.h): correct block byte sizes, deterministic (cache-hit-safe —
    // two calls on byte-identical input must produce byte-identical output), including sub-4x4 mip
    // tail levels, mirroring the BC7 mip-chain assertions above.
    {
        std::vector<std::byte> rgba(4 * 4 * 4, std::byte{0});
        for (usize texel = 0; texel < 16; ++texel) {
            rgba[texel * 4 + 0] = static_cast<std::byte>(texel * 8);        // R
            rgba[texel * 4 + 1] = static_cast<std::byte>(255 - texel * 8);  // G
            rgba[texel * 4 + 2] = static_cast<std::byte>(texel * 4);        // B
            rgba[texel * 4 + 3] = std::byte{255};                          // A
        }

        auto bc1_first = Engine::Detail::compress_bc1(rgba, 4, 4, false);
        assert(bc1_first.has_value());
        assert(bc1_first->size() == 8);
        auto bc1_second = Engine::Detail::compress_bc1(rgba, 4, 4, false);
        assert(bc1_second.has_value());
        assert(*bc1_first == *bc1_second);

        auto bc3_first = Engine::Detail::compress_bc3(rgba, 4, 4, false);
        assert(bc3_first.has_value());
        assert(bc3_first->size() == 16);
        auto bc3_second = Engine::Detail::compress_bc3(rgba, 4, 4, false);
        assert(bc3_second.has_value());
        assert(*bc3_first == *bc3_second);

        auto bc4_first = Engine::Detail::compress_bc4(rgba, 4, 4, 0);
        assert(bc4_first.has_value());
        assert(bc4_first->size() == 8);
        auto bc4_second = Engine::Detail::compress_bc4(rgba, 4, 4, 0);
        assert(bc4_second.has_value());
        assert(*bc4_first == *bc4_second);
        // A different channel selection on the SAME source pixels must not collide in the disk
        // cache with channel 0's result above (see BcCacheHeader's own comment).
        auto bc4_channel1 = Engine::Detail::compress_bc4(rgba, 4, 4, 1);
        assert(bc4_channel1.has_value());
        assert(*bc4_channel1 != *bc4_first);

        auto bc5_first = Engine::Detail::compress_bc5(rgba, 4, 4, 0, 1);
        assert(bc5_first.has_value());
        assert(bc5_first->size() == 16);
        auto bc5_second = Engine::Detail::compress_bc5(rgba, 4, 4, 0, 1);
        assert(bc5_second.has_value());
        assert(*bc5_first == *bc5_second);

        auto mips = Engine::Detail::generate_rgba8_mip_chain(rgba, 4, 4, false);
        assert(mips.has_value());
        auto bc1_mips = Engine::Detail::compress_bc1_mip_chain(mips->data, 4, 4, mips->mip_levels, false);
        assert(bc1_mips.has_value());
        assert(bc1_mips->size() == 3 * 8); // 4x4 + 2x2 + 1x1, 8 bytes/block each
        auto bc5_mips = Engine::Detail::compress_bc5_mip_chain(mips->data, 4, 4, mips->mip_levels);
        assert(bc5_mips.has_value());
        assert(bc5_mips->size() == 3 * 16);
    }

    // Detail::choose_bc_format: the "Compression Manager" policy table.
    {
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorAlpha, true) == RHI::Format::BC7UnormSrgb);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorAlpha, false) == RHI::Format::BC7Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorOpaque, true) == RHI::Format::BC1UnormSrgb);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorOpaque, false) == RHI::Format::BC1Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::Mask, false) == RHI::Format::BC4Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::NormalMap, false) == RHI::Format::BC5Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::MetallicRoughness, false) == RHI::Format::BC5Unorm);
    }

    // Detail::pack_orm_rgba8: exact R/G/B/A channel interleaving from two independent sources, and
    // a hard reject on mismatched dimensions rather than silently misaligning channels.
    {
        // 2x2 occlusion source: R = 10, 20, 30, 40 (only R matters -- G/B/A are irrelevant noise).
        const std::vector<std::byte> occlusion{
            std::byte{10}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{20}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{30}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{40}, std::byte{1}, std::byte{2}, std::byte{3},
        };
        // 2x2 metallic-roughness source: G = roughness (50, 60, 70, 80), B = metallic (90, 100, 110, 120).
        const std::vector<std::byte> metallic_roughness{
            std::byte{9}, std::byte{50}, std::byte{90}, std::byte{9},
            std::byte{9}, std::byte{60}, std::byte{100}, std::byte{9},
            std::byte{9}, std::byte{70}, std::byte{110}, std::byte{9},
            std::byte{9}, std::byte{80}, std::byte{120}, std::byte{9},
        };
        auto packed = Engine::Detail::pack_orm_rgba8(occlusion, metallic_roughness, 2, 2);
        assert(packed.has_value());
        assert(packed->size() == 16);
        for (usize texel = 0; texel < 4; ++texel) {
            assert(std::to_integer<u8>((*packed)[texel * 4 + 0]) == 10 + texel * 10); // R = occlusion
            assert(std::to_integer<u8>((*packed)[texel * 4 + 1]) == 50 + texel * 10); // G = roughness
            assert(std::to_integer<u8>((*packed)[texel * 4 + 2]) == 90 + texel * 10); // B = metallic
            assert(std::to_integer<u8>((*packed)[texel * 4 + 3]) == 255);            // A = opaque
        }

        // Mismatched dimensions must never silently misalign channels.
        const std::vector<std::byte> wrong_size(3 * 3 * 4, std::byte{0});
        assert(!Engine::Detail::pack_orm_rgba8(occlusion, wrong_size, 2, 2).has_value());
    }

    // Detail::pack_metallic_roughness_rg: G/B -> R/G re-channeling for the standalone-BC5 path
    // (TextureKind::MetallicRoughness). Must produce the correct layout regardless of whether BC5
    // compression itself later succeeds or falls back to uncompressed upload -- see that kind's own
    // doc comment for why the repack happens here rather than as a compress_bc5 channel argument.
    {
        // 2x2 metallic-roughness source: G = roughness (50, 60, 70, 80), B = metallic (90, 100, 110, 120).
        const std::vector<std::byte> metallic_roughness{
            std::byte{9}, std::byte{50}, std::byte{90}, std::byte{9},
            std::byte{9}, std::byte{60}, std::byte{100}, std::byte{9},
            std::byte{9}, std::byte{70}, std::byte{110}, std::byte{9},
            std::byte{9}, std::byte{80}, std::byte{120}, std::byte{9},
        };
        auto repacked = Engine::Detail::pack_metallic_roughness_rg(metallic_roughness, 2, 2);
        assert(repacked.has_value());
        assert(repacked->size() == 16);
        for (usize texel = 0; texel < 4; ++texel) {
            assert(std::to_integer<u8>((*repacked)[texel * 4 + 0]) == 50 + texel * 10); // R = roughness (orig G)
            assert(std::to_integer<u8>((*repacked)[texel * 4 + 1]) == 90 + texel * 10); // G = metallic (orig B)
        }

        // A repacked buffer fed straight into compress_bc5's default R/G channels must round-trip
        // through the same block size as any other BC5 use (NormalMap included).
        auto bc5 = Engine::Detail::compress_bc5(*repacked, 2, 2);
        assert(bc5.has_value());
        assert(bc5->size() == 16);

        assert(!Engine::Detail::pack_metallic_roughness_rg(metallic_roughness, 3, 3).has_value());
    }

    // Engine::Detail::compress_gdeflate_sibling's disk-content-hash cache: first call is a cache
    // miss (computes + writes), second call with byte-identical input is a cache hit (reads back) --
    // both must produce byte-identical compressed output, and both must decompress back to `source`.
    {
        constexpr u32 width = 256;
        constexpr u32 height = 64; // 256*64*4 == 64KiB == source.size(), keeps the two tests consistent
        constexpr bool srgb = true;

        auto first = Engine::Detail::compress_gdeflate_sibling(std::span<const std::byte>{source}, width, height, srgb);
        assert(first.has_value());

        auto second = Engine::Detail::compress_gdeflate_sibling(std::span<const std::byte>{source}, width, height, srgb);
        assert(second.has_value());
        assert(*first == *second);

        auto decompressed = Core::decompress_gdeflate(std::span<const std::byte>{*first}, source.size());
        assert(decompressed.has_value());
        assert(*decompressed == source);
    }

    return 0;
}
