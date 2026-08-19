#include <Engine/TextureCompression.hpp>

#include <Engine/AssetManager.hpp>

#include <Core/Decompression.hpp>

#include <RHI/RHI.hpp>

#include <bc7enc.h>

#define RGBCX_IMPLEMENTATION
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

namespace SFT::Engine::Detail {

    namespace {

        constexpr u64 kFnvOffsetBasis = 0xcbf29ce484222325ULL;
        constexpr u64 kFnvPrime = 0x100000001b3ULL;

        /// Performs the fnv1a append operation for `Detail` using the supplied arguments.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param hash `hash` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 fnv1a_append(std::span<const std::byte> data, u64 hash) noexcept {
            for (std::byte b : data) {
                hash ^= static_cast<u64>(b);
                hash *= kFnvPrime;
            }
            return hash;
        }

        /// Performs the fnv1a append pod operation for `Detail` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        /// @param hash `hash` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <typename T>
        [[nodiscard]] u64 fnv1a_append_pod(const T &value, u64 hash) noexcept {
            return fnv1a_append(std::as_bytes(std::span{&value, 1}), hash);
        }


        struct BcCacheHeader {
            std::array<char, 4> magic{};
            u32 version = 2;
            u32 width = 0;
            u32 height = 0;
            u8 srgb = 0;
            u8 channel0 = 0;
            u8 channel1 = 0;
            u8 reserved = 0;
            u64 block_bytes = 0;
        };

        /// Performs the bc content hash operation for `Detail` using the supplied arguments.
        ///
        /// @param rgba8 `rgba8` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        /// @param channel0 `channel0` value used by the operation.
        /// @param channel1 `channel1` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 bc_content_hash(std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb,
                                          u8 channel0, u8 channel1) noexcept {
            u64 hash = kFnvOffsetBasis;
            hash = fnv1a_append_pod(width, hash);
            hash = fnv1a_append_pod(height, hash);
            hash = fnv1a_append_pod(srgb, hash);
            hash = fnv1a_append_pod(channel0, hash);
            hash = fnv1a_append_pod(channel1, hash);
            hash = fnv1a_append(rgba8, hash);
            return hash;
        }

        /// Resolves the bc cache path associated with the supplied key, handle, or resource.
        ///
        /// @param extension `extension` value used by the operation.
        /// @param hash `hash` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::filesystem::path bc_cache_path_for(std::string_view extension, u64 hash) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
            return std::filesystem::path{".cache"} / "compressed_textures" /
                (std::string{hex} + "." + std::string{extension});
        }

        /// Returns the bc block data size for this `Detail`.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param bytes_per_block `bytes_per_block` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<usize> bc_block_data_size(u32 width, u32 height, u32 bytes_per_block) noexcept {
            const u64 blocks_wide = (static_cast<u64>(width) + 3) / 4;
            const u64 blocks_high = (static_cast<u64>(height) + 3) / 4;
            if (blocks_wide != 0 && blocks_high > std::numeric_limits<u64>::max() / blocks_wide) {
                return std::nullopt;
            }
            const u64 block_count = blocks_wide * blocks_high;
            if (block_count > std::numeric_limits<u64>::max() / bytes_per_block) {
                return std::nullopt;
            }
            const u64 bytes = block_count * bytes_per_block;
            if (bytes > std::numeric_limits<usize>::max()) {
                return std::nullopt;
            }
            return static_cast<usize>(bytes);
        }

        /// Reads bc cache from the associated source.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param magic `magic` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        /// @param channel0 `channel0` value used by the operation.
        /// @param channel1 `channel1` value used by the operation.
        /// @param expected_bytes `expected_bytes` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] std::optional<std::vector<std::byte>> read_bc_cache(
            const std::filesystem::path &path, std::array<char, 4> magic, u32 width, u32 height, bool srgb,
            u8 channel0, u8 channel1, usize expected_bytes) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            BcCacheHeader header{};
            file.read(reinterpret_cast<char *>(&header), sizeof(header));
            if (!file || header.magic != magic || header.version != 2 || header.width != width ||
                header.height != height || header.srgb != (srgb ? 1u : 0u) || header.channel0 != channel0 ||
                header.channel1 != channel1 || header.block_bytes != expected_bytes) {
                return std::nullopt;
            }
            std::vector<std::byte> blocks(expected_bytes);
            file.read(reinterpret_cast<char *>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
            if (!file || file.peek() != std::char_traits<char>::eof()) {
                return std::nullopt;
            }
            return blocks;
        }


        /// Writes bc cache to the associated destination.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param magic `magic` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        /// @param channel0 `channel0` value used by the operation.
        /// @param channel1 `channel1` value used by the operation.
        /// @param blocks `blocks` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void write_bc_cache(const std::filesystem::path &path, std::array<char, 4> magic, u32 width, u32 height,
                            bool srgb, u8 channel0, u8 channel1, std::span<const std::byte> blocks) noexcept {
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
                const BcCacheHeader header{
                    .magic = magic,
                    .width = width,
                    .height = height,
                    .srgb = static_cast<u8>(srgb ? 1 : 0),
                    .channel0 = channel0,
                    .channel1 = channel1,
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


        /// Performs the extract rgba8 block operation for `Detail` using the supplied arguments.
        ///
        /// @param rgba8 `rgba8` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param bx `bx` value used by the operation.
        /// @param by `by` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::array<std::byte, 16 * 4> extract_rgba8_block(
            std::span<const std::byte> rgba8, u32 width, u32 height, u32 bx, u32 by) noexcept {
            std::array<std::byte, 16 * 4> block_pixels{};
            for (u32 row = 0; row < 4; ++row) {
                const u32 src_y = std::min(by * 4 + row, height - 1);
                for (u32 column = 0; column < 4; ++column) {
                    const u32 src_x = std::min(bx * 4 + column, width - 1);
                    const usize src_offset = (static_cast<usize>(src_y) * width + src_x) * 4;
                    const usize block_offset = (static_cast<usize>(row) * 4 + column) * 4;
                    std::memcpy(&block_pixels[block_offset], &rgba8[src_offset], 4);
                }
            }
            return block_pixels;
        }


        template <typename EncodeBlockFn>
        [[nodiscard]] std::optional<std::vector<std::byte>> encode_bc_blocks(
            std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb, u8 channel0, u8 channel1,
            u32 bytes_per_block, std::array<char, 4> magic, std::string_view extension,
            EncodeBlockFn &&encode_block) {
            const u64 source_texels = static_cast<u64>(width) * height;
            if (width == 0 || height == 0 || source_texels > std::numeric_limits<u64>::max() / 4u) {
                return std::nullopt;
            }
            const u64 source_bytes = source_texels * 4u;
            const std::optional<usize> block_bytes = bc_block_data_size(width, height, bytes_per_block);
            if (source_bytes > std::numeric_limits<usize>::max() ||
                rgba8.size() != static_cast<usize>(source_bytes) || !block_bytes) {
                return std::nullopt;
            }

            const u64 hash = bc_content_hash(rgba8, width, height, srgb, channel0, channel1);
            const std::filesystem::path path = bc_cache_path_for(extension, hash);
            if (std::optional<std::vector<std::byte>> cached =
                    read_bc_cache(path, magic, width, height, srgb, channel0, channel1, *block_bytes)) {
                return cached;
            }

            const u32 blocks_wide = (width + 3) / 4;
            const u32 blocks_high = (height + 3) / 4;
            std::vector<std::byte> blocks(*block_bytes);
            for (u32 by = 0; by < blocks_high; ++by) {
                for (u32 bx = 0; bx < blocks_wide; ++bx) {
                    const std::array<std::byte, 16 * 4> block_pixels =
                        extract_rgba8_block(rgba8, width, height, bx, by);
                    const usize block_offset = (static_cast<usize>(by) * blocks_wide + bx) * bytes_per_block;
                    encode_block(&blocks[block_offset], block_pixels.data());
                }
            }

            write_bc_cache(path, magic, width, height, srgb, channel0, channel1, blocks);
            return blocks;
        }


        template <typename CompressLevelFn>
        [[nodiscard]] std::optional<std::vector<std::byte>> compress_mip_chain(
            std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels,
            CompressLevelFn &&compress_level) {
            if (width == 0 || height == 0 || mip_levels == 0) {
                return std::nullopt;
            }

            std::vector<std::byte> blocks;
            usize rgba_offset = 0;
            for (u32 level = 0; level < mip_levels; ++level) {
                const u64 level_texels = static_cast<u64>(width) * height;
                if (level_texels > std::numeric_limits<u64>::max() / 4u) {
                    return std::nullopt;
                }
                const u64 level_bytes_u64 = level_texels * 4u;
                if (level_bytes_u64 > std::numeric_limits<usize>::max()) {
                    return std::nullopt;
                }
                const usize level_bytes = static_cast<usize>(level_bytes_u64);
                if (rgba_offset > rgba8_mips.size() || level_bytes > rgba8_mips.size() - rgba_offset) {
                    return std::nullopt;
                }

                auto level_blocks = compress_level(rgba8_mips.subspan(rgba_offset, level_bytes), width, height);
                if (!level_blocks) {
                    return std::nullopt;
                }
                blocks.insert(blocks.end(), level_blocks->begin(), level_blocks->end());
                rgba_offset += level_bytes;

                if (level + 1u < mip_levels) {
                    if (width == 1 && height == 1) {
                        return std::nullopt;
                    }
                    width = std::max(width / 2u, 1u);
                    height = std::max(height / 2u, 1u);
                }
            }
            if (rgba_offset != rgba8_mips.size()) {
                return std::nullopt;
            }
            return blocks;
        }

        /// Finds or creates the bc7enc initialized required by the operation.
        ///
        /// @note This function does not throw exceptions.
        void ensure_bc7enc_initialized() noexcept {
            static std::once_flag once;
            std::call_once(once, [] { bc7enc_compress_block_init(); });
        }


        /// Finds or creates the rgbcx initialized required by the operation.
        ///
        /// @note This function does not throw exceptions.
        void ensure_rgbcx_initialized() noexcept {
            static std::once_flag once;
            std::call_once(once, [] { rgbcx::init(); });
        }


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

        /// Computes the content hash bytes required by the supplied values.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 content_hash_bytes(std::span<const std::byte> data, u32 width, u32 height,
                                             bool srgb) noexcept {
            u64 hash = kFnvOffsetBasis;
            hash = fnv1a_append_pod(width, hash);
            hash = fnv1a_append_pod(height, hash);
            hash = fnv1a_append_pod(srgb, hash);
            hash = fnv1a_append(data, hash);
            return hash;
        }

        /// Resolves the gdeflate cache path associated with the supplied key, handle, or resource.
        ///
        /// @param hash `hash` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::filesystem::path gdeflate_cache_path_for(u64 hash) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
            return std::filesystem::path{".cache"} / "compressed_textures" / (std::string{hex} + ".sgdf");
        }

        /// Reads gdeflate cache from the associated source.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        /// @param expected_decompressed_size Requested or available size for the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
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

        /// Writes gdeflate cache to the associated destination.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param srgb `srgb` value used by the operation.
        /// @param decompressed_size Requested or available size for the operation.
        /// @param compressed `compressed` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
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

    /// Compresses bc7 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc7(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        bool srgb) {
        ensure_bc7enc_initialized();
        bc7enc_compress_block_params params{};
        bc7enc_compress_block_params_init(&params);
        params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
        params.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;

        return encode_bc_blocks(rgba8, width, height, srgb, 0, 0, BC7ENC_BLOCK_SIZE, {'S', 'B', 'C', '7'}, "sbc7",
            [&params](void *dst, const std::byte *pixels) { bc7enc_compress_block(dst, pixels, &params); });
    }

    /// Compresses bc7 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc7_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb) {
        return compress_mip_chain(rgba8_mips, width, height, mip_levels,
            [srgb](std::span<const std::byte> level, u32 w, u32 h) { return compress_bc7(level, w, h, srgb); });
    }

    /// Compresses bc1 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc1(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        bool srgb) {
        ensure_rgbcx_initialized();
        return encode_bc_blocks(rgba8, width, height, srgb, 0, 0, 8, {'S', 'B', 'C', '1'}, "sbc1",
            [](void *dst, const std::byte *pixels) {
                rgbcx::encode_bc1(rgbcx::MAX_LEVEL, dst, reinterpret_cast<const uint8_t *>(pixels),
                                     true,                                      false);
            });
    }

    /// Compresses bc1 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc1_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb) {
        return compress_mip_chain(rgba8_mips, width, height, mip_levels,
            [srgb](std::span<const std::byte> level, u32 w, u32 h) { return compress_bc1(level, w, h, srgb); });
    }

    /// Compresses bc3 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc3(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        bool srgb) {
        ensure_rgbcx_initialized();
        return encode_bc_blocks(rgba8, width, height, srgb, 0, 0, 16, {'S', 'B', 'C', '3'}, "sbc3",
            [](void *dst, const std::byte *pixels) {
                rgbcx::encode_bc3(rgbcx::MAX_LEVEL, dst, reinterpret_cast<const uint8_t *>(pixels));
            });
    }

    /// Compresses bc3 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc3_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb) {
        return compress_mip_chain(rgba8_mips, width, height, mip_levels,
            [srgb](std::span<const std::byte> level, u32 w, u32 h) { return compress_bc3(level, w, h, srgb); });
    }

    /// Compresses bc4 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<std::vector<std::byte>> compress_bc4(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        u32 channel) {
        if (channel > 3) {
            return std::nullopt;
        }
        ensure_rgbcx_initialized();
        return encode_bc_blocks(rgba8, width, height, false, static_cast<u8>(channel), 0, 8,
            {'S', 'B', 'C', '4'}, "sbc4",
            [channel](void *dst, const std::byte *pixels) {
                rgbcx::encode_bc4(dst, reinterpret_cast<const uint8_t *>(pixels) + channel, 4);
            });
    }

    /// Compresses bc4 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc4_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, u32 channel) {
        return compress_mip_chain(rgba8_mips, width, height, mip_levels,
            [channel](std::span<const std::byte> level, u32 w, u32 h) { return compress_bc4(level, w, h, channel); });
    }

    /// Compresses bc5 into the requested representation.
    ///
    /// @param rgba8 `rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param channel0 `channel0` value used by the operation.
    /// @param channel1 `channel1` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<std::vector<std::byte>> compress_bc5(std::span<const std::byte> rgba8, u32 width, u32 height,
                                                        u32 channel0, u32 channel1) {
        if (channel0 > 3 || channel1 > 3) {
            return std::nullopt;
        }
        ensure_rgbcx_initialized();
        return encode_bc_blocks(rgba8, width, height, false, static_cast<u8>(channel0), static_cast<u8>(channel1),
            16, {'S', 'B', 'C', '5'}, "sbc5",
            [channel0, channel1](void *dst, const std::byte *pixels) {
                rgbcx::encode_bc5(dst, reinterpret_cast<const uint8_t *>(pixels), channel0, channel1, 4);
            });
    }

    /// Compresses bc5 mip chain into the requested representation.
    ///
    /// @param rgba8_mips `rgba8_mips` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    /// @param channel0 `channel0` value used by the operation.
    /// @param channel1 `channel1` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<std::vector<std::byte>> compress_bc5_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, u32 channel0, u32 channel1) {
        return compress_mip_chain(rgba8_mips, width, height, mip_levels,
            [channel0, channel1](std::span<const std::byte> level, u32 w, u32 h) {
                return compress_bc5(level, w, h, channel0, channel1);
            });
    }

    /// Compresses gdeflate sibling into the requested representation.
    ///
    /// @param bc7_blocks `bc7_blocks` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
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

    /// Selects bc format that best satisfies the supplied requirements.
    ///
    /// @param kind `kind` value used by the operation.
    /// @param srgb `srgb` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RHI::Format choose_bc_format(TextureKind kind, bool srgb) noexcept {
        switch (kind) {
            case TextureKind::ColorOpaque: return srgb ? RHI::Format::BC1UnormSrgb : RHI::Format::BC1Unorm;
            case TextureKind::Mask: return RHI::Format::BC4Unorm;
            case TextureKind::NormalMap:
            case TextureKind::MetallicRoughness: return RHI::Format::BC5Unorm;
            case TextureKind::ColorAlpha:
            default: return srgb ? RHI::Format::BC7UnormSrgb : RHI::Format::BC7Unorm;
        }
    }

    /// Packs metallic roughness rg using the supplied arguments and current state.
    ///
    /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<std::vector<std::byte>> pack_metallic_roughness_rg(
        std::span<const std::byte> metallic_roughness_rgba8, u32 width, u32 height) {
        if (width == 0 || height == 0) {
            return std::nullopt;
        }
        const u64 texels = static_cast<u64>(width) * height;
        if (texels > std::numeric_limits<u64>::max() / 4u) {
            return std::nullopt;
        }
        const u64 bytes_u64 = texels * 4u;
        if (bytes_u64 > std::numeric_limits<usize>::max()) {
            return std::nullopt;
        }
        const usize bytes = static_cast<usize>(bytes_u64);
        if (metallic_roughness_rgba8.size() != bytes) {
            return std::nullopt;
        }

        std::vector<std::byte> repacked(bytes);
        for (usize texel = 0; texel < static_cast<usize>(texels); ++texel) {
            const usize offset = texel * 4;
            repacked[offset + 0] = metallic_roughness_rgba8[offset + 1];
            repacked[offset + 1] = metallic_roughness_rgba8[offset + 2];
            repacked[offset + 2] = std::byte{0};
            repacked[offset + 3] = std::byte{255};
        }
        return repacked;
    }

    /// Packs orm rgba8 using the supplied arguments and current state.
    ///
    /// @param occlusion_rgba8 `occlusion_rgba8` value used by the operation.
    /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    std::optional<std::vector<std::byte>> pack_orm_rgba8(std::span<const std::byte> occlusion_rgba8,
                                                          std::span<const std::byte> metallic_roughness_rgba8,
                                                          u32 width, u32 height) {
        if (width == 0 || height == 0) {
            return std::nullopt;
        }
        const u64 texels = static_cast<u64>(width) * height;
        if (texels > std::numeric_limits<u64>::max() / 4u) {
            return std::nullopt;
        }
        const u64 bytes_u64 = texels * 4u;
        if (bytes_u64 > std::numeric_limits<usize>::max()) {
            return std::nullopt;
        }
        const usize bytes = static_cast<usize>(bytes_u64);
        if (occlusion_rgba8.size() != bytes || metallic_roughness_rgba8.size() != bytes) {
            return std::nullopt;
        }

        std::vector<std::byte> packed(bytes);
        for (usize texel = 0; texel < static_cast<usize>(texels); ++texel) {
            const usize offset = texel * 4;
            packed[offset + 0] = occlusion_rgba8[offset + 0];
            packed[offset + 1] = metallic_roughness_rgba8[offset + 1];
            packed[offset + 2] = metallic_roughness_rgba8[offset + 2];
            packed[offset + 3] = std::byte{255};
        }
        return packed;
    }

} // namespace SFT::Engine::Detail
