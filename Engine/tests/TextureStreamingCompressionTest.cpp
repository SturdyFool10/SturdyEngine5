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

    return 0;
}
