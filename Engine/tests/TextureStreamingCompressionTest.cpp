#include <Engine/TextureCompression.hpp>
#include <Engine/TextureMipChain.hpp>

#include <Core/src/Core/Decompression.hpp>

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
