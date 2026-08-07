#include "TextureCompression.hpp"

#include <Core/src/Core/Decompression.hpp>

#include <bc7enc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>

namespace SFT::Engine::Detail {

    namespace {

        constexpr u64 kFnvOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr u64 kFnvPrime = 0x100000001b3ULL;

        [[nodiscard]] u64 fnv1a_append(std::span<const std::byte> data, u64 hash) noexcept {
            for (std::byte b : data) {
                hash ^= static_cast<u64>(b);
                hash *= kFnvPrime;
            }
            return hash;
        }

        template <typename T>
        [[nodiscard]] u64 fnv1a_append_pod(const T &value, u64 hash) noexcept {
            return fnv1a_append(std::as_bytes(std::span{&value, 1}), hash);
        }

        [[nodiscard]] u64 content_hash(std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb) noexcept {
            u64 hash = kFnvOffsetBasis;
            hash = fnv1a_append_pod(width, hash);
            hash = fnv1a_append_pod(height, hash);
            hash = fnv1a_append_pod(srgb, hash);
            hash = fnv1a_append(rgba8, hash);
            return hash;
        }

        // Deliberately not a portable/versioned-forever format — this is a purely local,
        // regenerate-on-miss cache (see compress_bc7's own doc comment), not shipped data.
        struct CacheHeader {
            std::array<char, 4> magic{'S', 'B', 'C', '7'};
            u32 version = 1;
            u32 width = 0;
            u32 height = 0;
            u8 srgb = 0;
            std::array<u8, 3> reserved{};
            u64 block_bytes = 0;
        };

        [[nodiscard]] std::filesystem::path cache_path_for(u64 hash) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
            return std::filesystem::path{".cache"} / "compressed_textures" / (std::string{hex} + ".sbc7");
        }

        [[nodiscard]] std::optional<usize> bc7_data_size(u32 width, u32 height) noexcept {
            const u64 blocks_wide = (static_cast<u64>(width) + 3) / 4;
            const u64 blocks_high = (static_cast<u64>(height) + 3) / 4;
            if (blocks_wide != 0 && blocks_high > std::numeric_limits<u64>::max() / blocks_wide) {
                return std::nullopt;
            }
            const u64 block_count = blocks_wide * blocks_high;
            if (block_count > std::numeric_limits<u64>::max() / BC7ENC_BLOCK_SIZE) {
                return std::nullopt;
            }
            const u64 bytes = block_count * BC7ENC_BLOCK_SIZE;
            if (bytes > std::numeric_limits<usize>::max()) {
                return std::nullopt;
            }
            return static_cast<usize>(bytes);
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> read_cache(const std::filesystem::path &path,
                                                                       u32 width, u32 height, bool srgb) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            CacheHeader header{};
            file.read(reinterpret_cast<char *>(&header), sizeof(header));
            const std::optional<usize> expected_bytes = bc7_data_size(width, height);
            if (!file || !expected_bytes || header.magic != std::array<char, 4>{'S', 'B', 'C', '7'} ||
                header.version != 1 || header.width != width || header.height != height ||
                header.srgb != (srgb ? 1u : 0u) || header.block_bytes != *expected_bytes) {
                return std::nullopt;
            }
            std::vector<std::byte> blocks(*expected_bytes);
            file.read(reinterpret_cast<char *>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
            if (!file || file.peek() != std::char_traits<char>::eof()) {
                return std::nullopt;
            }
            return blocks;
        }

        // Atomic (temp-file + rename) best-effort write — a failure here just means the next load
        // re-encodes instead of hitting the cache, never a hard error for the caller.
        void write_cache(const std::filesystem::path &path, u32 width, u32 height, bool srgb,
                         std::span<const std::byte> blocks) noexcept {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return;
            }
            const std::filesystem::path temp_path = path.string() + ".tmp";
            {
                std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    return;
                }
                const CacheHeader header{
                    .width = width,
                    .height = height,
                    .srgb = static_cast<u8>(srgb ? 1 : 0),
                    .block_bytes = blocks.size(),
                };
                file.write(reinterpret_cast<const char *>(&header), sizeof(header));
                file.write(reinterpret_cast<const char *>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
                if (!file) {
                    return;
                }
            }
            std::filesystem::rename(temp_path, path, ec);
            if (ec) {
                std::filesystem::remove(temp_path, ec);
            }
        }


        void ensure_bc7enc_initialized() noexcept {
            static std::once_flag once;
            std::call_once(once, [] { bc7enc_compress_block_init(); });
        }

        // Same "deliberately not a portable/versioned-forever format" caveat as CacheHeader above --
        // a local regenerate-on-miss cache, not shipped data. `decompressed_size` (the original
        // bc7_blocks size) is stored here because GDeflate's own container doesn't self-describe it
        // (see Core::decompress_gdeflate's doc comment) -- without it, reading this cache back would
        // have nothing to pass as decompress_gdeflate's required exact-size argument.
        struct GDeflateCacheHeader {
            std::array<char, 4> magic{'S', 'G', 'D', 'F'};
            u32 version = 1;
            u32 width = 0;
            u32 height = 0;
            u8 srgb = 0;
            std::array<u8, 3> reserved{};
            u64 decompressed_size = 0;
            u64 compressed_size = 0;
        };

        [[nodiscard]] u64 content_hash_bytes(std::span<const std::byte> data, u32 width, u32 height,
                                             bool srgb) noexcept {
            u64 hash = kFnvOffsetBasis;
            hash = fnv1a_append_pod(width, hash);
            hash = fnv1a_append_pod(height, hash);
            hash = fnv1a_append_pod(srgb, hash);
            hash = fnv1a_append(data, hash);
            return hash;
        }

        [[nodiscard]] std::filesystem::path gdeflate_cache_path_for(u64 hash) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
            return std::filesystem::path{".cache"} / "compressed_textures" / (std::string{hex} + ".sgdf");
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> read_gdeflate_cache(
            const std::filesystem::path &path, u32 width, u32 height, bool srgb, u64 expected_decompressed_size) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            GDeflateCacheHeader header{};
            file.read(reinterpret_cast<char *>(&header), sizeof(header));
            if (!file || header.magic != std::array<char, 4>{'S', 'G', 'D', 'F'} || header.version != 1 ||
                header.width != width || header.height != height || header.srgb != (srgb ? 1u : 0u) ||
                header.decompressed_size != expected_decompressed_size) {
                return std::nullopt;
            }
            std::vector<std::byte> compressed(static_cast<usize>(header.compressed_size));
            file.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
            if (!file || file.peek() != std::char_traits<char>::eof()) {
                return std::nullopt;
            }
            return compressed;
        }

        void write_gdeflate_cache(const std::filesystem::path &path, u32 width, u32 height, bool srgb,
                                  u64 decompressed_size, std::span<const std::byte> compressed) noexcept {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return;
            }
            const std::filesystem::path temp_path = path.string() + ".tmp";
            {
                std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    return;
                }
                const GDeflateCacheHeader header{
                    .width = width,
                    .height = height,
                    .srgb = static_cast<u8>(srgb ? 1 : 0),
                    .decompressed_size = decompressed_size,
                    .compressed_size = compressed.size(),
                };
                file.write(reinterpret_cast<const char *>(&header), sizeof(header));
                file.write(reinterpret_cast<const char *>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
                if (!file) {
                    return;
                }
            }
            std::filesystem::rename(temp_path, path, ec);
            if (ec) {
                std::filesystem::remove(temp_path, ec);
            }
        }

    } // namespace

    std::optional<std::vector<std::byte>> compress_bc7(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        bool srgb) {
        const u64 source_bytes = static_cast<u64>(width) * height * 4;
        const std::optional<usize> block_bytes = bc7_data_size(width, height);
        if (width < 4 || height < 4 || source_bytes > std::numeric_limits<usize>::max() ||
            rgba8.size() != static_cast<usize>(source_bytes) || !block_bytes) {
            return std::nullopt;
        }

        const u64 hash = content_hash(rgba8, width, height, srgb);
        const std::filesystem::path path = cache_path_for(hash);
        if (std::optional<std::vector<std::byte>> cached = read_cache(path, width, height, srgb)) {
            return cached;
        }

        const u32 blocks_wide = (width + 3) / 4;
        const u32 blocks_high = (height + 3) / 4;

        ensure_bc7enc_initialized();
        bc7enc_compress_block_params params{};
        bc7enc_compress_block_params_init(&params);
        params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
        params.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;

        std::vector<std::byte> blocks(*block_bytes);
        std::array<std::byte, 16 * 4> block_pixels{};
        for (u32 by = 0; by < blocks_high; ++by) {
            for (u32 bx = 0; bx < blocks_wide; ++bx) {
                for (u32 row = 0; row < 4; ++row) {
                    const u32 src_y = std::min(by * 4 + row, height - 1);
                    for (u32 column = 0; column < 4; ++column) {
                        const u32 src_x = std::min(bx * 4 + column, width - 1);
                        const usize src_offset = (static_cast<usize>(src_y) * width + src_x) * 4;
                        const usize block_offset = (static_cast<usize>(row) * 4 + column) * 4;
                        std::memcpy(&block_pixels[block_offset], &rgba8[src_offset], 4);
                    }
                }
                const usize block_offset = (static_cast<usize>(by) * blocks_wide + bx) * BC7ENC_BLOCK_SIZE;
                bc7enc_compress_block(&blocks[block_offset], block_pixels.data(), &params);
            }
        }

        write_cache(path, width, height, srgb, blocks);
        return blocks;
    }

    std::optional<std::vector<std::byte>> compress_gdeflate_sibling(std::span<const std::byte> bc7_blocks,
                                                                     u32 width, u32 height, bool srgb) {
        if (bc7_blocks.empty()) {
            return std::nullopt;
        }

        const u64 hash = content_hash_bytes(bc7_blocks, width, height, srgb);
        const std::filesystem::path path = gdeflate_cache_path_for(hash);
        if (std::optional<std::vector<std::byte>> cached =
                read_gdeflate_cache(path, width, height, srgb, static_cast<u64>(bc7_blocks.size()))) {
            return cached;
        }

        auto compressed = Core::compress_gdeflate(bc7_blocks);
        if (!compressed) {
            return std::nullopt;
        }
        write_gdeflate_cache(path, width, height, srgb, static_cast<u64>(bc7_blocks.size()), *compressed);
        return std::move(*compressed);
    }

} // namespace SFT::Engine::Detail
