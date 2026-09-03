#include <Engine/AssetManager.hpp>
#include <Engine/TextureCompression.hpp>
#include <Engine/HdrTransfer.hpp>
#include <Engine/TextureMipChain.hpp>

#include <Core/Decompression.hpp>

#include <RHI/RHI.hpp>

#include <Renderer/RendererModule.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <random>
#include <vector>

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @pre `srgb_mips.has_value()`; debug builds assert if this precondition is violated.
/// @pre `srgb_mips->mip_levels == 2`; debug builds assert if this precondition is violated.
/// @pre `srgb_mips->data.size() == checker.size() + 4`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    namespace Core = SFT::Core;
    namespace Engine = SFT::Engine;
    namespace RHI = SFT::RHI;
    using SFT::f32;
    using SFT::u8;
    using SFT::u16;
    using SFT::u32;
    using SFT::usize;

    std::vector<std::byte> source(64 * 1024);
    for (usize i = 0; i < source.size(); ++i) {
        source[i] = static_cast<std::byte>((i / 37) % 251);
    }


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


    {
        std::vector<std::byte> rgba(4 * 4 * 4, std::byte{127});
        auto mips = Engine::Detail::generate_rgba8_mip_chain(rgba, 4, 4, false);
        assert(mips.has_value());
        auto bc7 = Engine::Detail::compress_bc7_mip_chain(mips->data, 4, 4, mips->mip_levels, false);
        assert(bc7.has_value());
        assert(bc7->size() == 3 * 16);
    }


    {
        auto compressed = Core::compress_gdeflate(std::span<const std::byte>{source});
        assert(compressed.has_value());
        assert(!compressed->empty());

        auto decompressed = Core::decompress_gdeflate(std::span<const std::byte>{*compressed}, source.size());
        assert(decompressed.has_value());
        assert(decompressed->size() == source.size());
        assert(*decompressed == source);
    }


    {
        std::vector<std::byte> rgba(4 * 4 * 4, std::byte{0});
        for (usize texel = 0; texel < 16; ++texel) {
            rgba[texel * 4 + 0] = static_cast<std::byte>(texel * 8);
            rgba[texel * 4 + 1] = static_cast<std::byte>(255 - texel * 8);
            rgba[texel * 4 + 2] = static_cast<std::byte>(texel * 4);
            rgba[texel * 4 + 3] = std::byte{255};
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
        assert(bc1_mips->size() == 3 * 8);
        auto bc5_mips = Engine::Detail::compress_bc5_mip_chain(mips->data, 4, 4, mips->mip_levels);
        assert(bc5_mips.has_value());
        assert(bc5_mips->size() == 3 * 16);
    }


    {
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorAlpha, true) == RHI::Format::BC7UnormSrgb);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorAlpha, false) == RHI::Format::BC7Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorOpaque, true) == RHI::Format::BC1UnormSrgb);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::ColorOpaque, false) == RHI::Format::BC1Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::Mask, false) == RHI::Format::BC4Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::NormalMap, false) == RHI::Format::BC5Unorm);
        assert(Engine::Detail::choose_bc_format(Engine::TextureKind::MetallicRoughness, false) == RHI::Format::BC5Unorm);
    }


    {

        const std::vector<std::byte> occlusion{
            std::byte{10}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{20}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{30}, std::byte{1}, std::byte{2}, std::byte{3},
            std::byte{40}, std::byte{1}, std::byte{2}, std::byte{3},
        };

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
            assert(std::to_integer<u8>((*packed)[texel * 4 + 0]) == 10 + texel * 10);
            assert(std::to_integer<u8>((*packed)[texel * 4 + 1]) == 50 + texel * 10);
            assert(std::to_integer<u8>((*packed)[texel * 4 + 2]) == 90 + texel * 10);
            assert(std::to_integer<u8>((*packed)[texel * 4 + 3]) == 255);
        }


        const std::vector<std::byte> wrong_size(3 * 3 * 4, std::byte{0});
        assert(!Engine::Detail::pack_orm_rgba8(occlusion, wrong_size, 2, 2).has_value());
    }


    {

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
            assert(std::to_integer<u8>((*repacked)[texel * 4 + 0]) == 50 + texel * 10);
            assert(std::to_integer<u8>((*repacked)[texel * 4 + 1]) == 90 + texel * 10);
        }


        auto bc5 = Engine::Detail::compress_bc5(*repacked, 2, 2);
        assert(bc5.has_value());
        assert(bc5->size() == 16);

        assert(!Engine::Detail::pack_metallic_roughness_rg(metallic_roughness, 3, 3).has_value());
    }


    {
        constexpr u32 width = 256;
        constexpr u32 height = 64;
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


    // --- RGBA16Float mip chain: the path an HDR source texture takes. The point of the format is
    //     that highlights survive, so the properties worth pinning are that the average is taken
    //     in linear light and that nothing is clamped on the way down the chain. ---
    {
        const auto half = [](f32 value) { return Engine::Detail::float_to_half(value); };
        const auto value_at = [](const std::vector<std::byte> &data, usize texel, usize channel) {
            u16 bits = 0;
            std::memcpy(&bits, data.data() + (texel * 4 + channel) * sizeof(u16), sizeof(bits));
            return Engine::Detail::half_to_float(bits);
        };

        // A 2x2 whose red channel is 8.0 in one texel and 0.0 in the other three -- an HDR
        // highlight well outside anything an 8-bit texture could hold.
        std::vector<std::byte> hdr(2 * 2 * 8);
        const auto put = [&](usize texel, f32 r, f32 g, f32 b, f32 a) {
            const f32 channels[4]{r, g, b, a};
            for (usize c = 0; c < 4; ++c) {
                const u16 bits = half(channels[c]);
                std::memcpy(hdr.data() + (texel * 4 + c) * sizeof(u16), &bits, sizeof(bits));
            }
        };
        put(0, 8.0f, 0.0f, 0.0f, 1.0f);
        put(1, 0.0f, 0.0f, 0.0f, 1.0f);
        put(2, 0.0f, 0.0f, 0.0f, 1.0f);
        put(3, 0.0f, 0.0f, 0.0f, 1.0f);

        auto mips = Engine::Detail::generate_rgba16f_mip_chain(hdr, 2, 2);
        assert(mips.has_value());
        assert(mips->mip_levels == 2);
        // Level 0 (4 texels) plus level 1 (1 texel), at 8 bytes each.
        assert(mips->data.size() == (4u + 1u) * 8u);

        // The base level must be copied verbatim, including the out-of-range highlight.
        assert(value_at(mips->data, 0, 0) == 8.0f);
        assert(value_at(mips->data, 0, 3) == 1.0f);

        // Level 1 is the average of the four: 8.0 / 4 == 2.0. Still above 1.0, which is exactly
        // what an 8-bit chain could not have represented -- it would have clamped the source texel
        // to 1.0 first and produced 0.25 here.
        assert(value_at(mips->data, 4, 0) == 2.0f);
        assert(value_at(mips->data, 4, 1) == 0.0f);
        assert(value_at(mips->data, 4, 3) == 1.0f);
    }

    {
        // Chain size and level count for a larger, non-square, non-power-of-two extent, against
        // the same level count the 8-bit path reports for the same dimensions.
        const u32 width = 5;
        const u32 height = 3;
        std::vector<std::byte> pixels(static_cast<usize>(width) * height * 8, std::byte{0});
        auto mips = Engine::Detail::generate_rgba16f_mip_chain(pixels, width, height);
        assert(mips.has_value());
        assert(mips->mip_levels == Engine::Detail::texture_mip_level_count(width, height));

        usize expected = 0;
        u32 level_width = width;
        u32 level_height = height;
        while (true) {
            expected += static_cast<usize>(level_width) * level_height * 8u;
            if (level_width == 1 && level_height == 1) {
                break;
            }
            level_width = std::max(level_width / 2u, 1u);
            level_height = std::max(level_height / 2u, 1u);
        }
        assert(mips->data.size() == expected);

        // A buffer whose length does not match the stated extent is rejected rather than read past.
        std::vector<std::byte> truncated(pixels.size() - 8);
        assert(!Engine::Detail::generate_rgba16f_mip_chain(truncated, width, height).has_value());
        assert(!Engine::Detail::generate_rgba16f_mip_chain(pixels, 0, height).has_value());
    }


    // --- The contract between the two halves of the texture upload path: whatever the Engine's
    //     mip generators produce, Renderer::create_texture must expect exactly that many bytes.
    //     It rejects a buffer whose size disagrees, so a mismatch here is a hard runtime failure
    //     for every texture of that format -- which is precisely how RGBA16Float was broken before
    //     this, being present in RHI::Format but in neither of the renderer's size tables. ---
    {
        struct Case {
            u32 width;
            u32 height;
        };
        for (const Case &c : {Case{1, 1}, Case{2, 2}, Case{4, 4}, Case{5, 3}, Case{64, 16}, Case{17, 39}}) {
            std::vector<std::byte> rgba8(static_cast<usize>(c.width) * c.height * 4u, std::byte{128});
            auto srgb_chain = Engine::Detail::generate_rgba8_mip_chain(rgba8, c.width, c.height, true);
            assert(srgb_chain.has_value());
            assert(srgb_chain->data.size() == SFT::Renderer::texture_mip_chain_byte_size(
                                                  RHI::Format::RGBA8UnormSrgb, c.width, c.height,
                                                  srgb_chain->mip_levels));

            auto linear_chain = Engine::Detail::generate_rgba8_mip_chain(rgba8, c.width, c.height, false);
            assert(linear_chain.has_value());
            assert(linear_chain->data.size() == SFT::Renderer::texture_mip_chain_byte_size(
                                                    RHI::Format::RGBA8Unorm, c.width, c.height,
                                                    linear_chain->mip_levels));

            std::vector<std::byte> rgba16f(static_cast<usize>(c.width) * c.height * 8u, std::byte{0});
            auto hdr_chain = Engine::Detail::generate_rgba16f_mip_chain(rgba16f, c.width, c.height);
            assert(hdr_chain.has_value());
            const SFT::u64 expected = SFT::Renderer::texture_mip_chain_byte_size(
                RHI::Format::RGBA16Float, c.width, c.height, hdr_chain->mip_levels);
            // Non-zero is the part that regressed: an unsupported format reports 0 here, and
            // create_texture turns that into "unsupported texture format".
            assert(expected != 0);
            assert(hdr_chain->data.size() == expected);
        }
    }

    return 0;
}
