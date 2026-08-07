#include <Engine/TextureCompression.hpp>

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
