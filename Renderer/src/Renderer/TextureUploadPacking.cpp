#include <Renderer/TextureUploadPacking.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace SFT::Renderer {

    namespace {

        /// Returns how many rows of packed data one mip level occupies.
        ///
        /// A block-compressed level stores one row per *block* row, covering four pixel rows, so
        /// walking pixel rows would read four times the data that is actually there. Uncompressed
        /// formats store one row per pixel row, where the two agree.
        ///
        /// @param format Texture format.
        /// @param height Pixel height of the level.
        ///
        /// @return Returns the number of stored rows.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 texture_row_count(RHI::Format format, u32 height) noexcept {
            if (height == 0) {
                return 0;
            }
            return RHI::format_is_block_compressed(format) ? (height + 3u) / 4u : height;
        }

        /// Returns the number of bytes one mip level of `width` by `height` occupies in `format`,
        /// tightly packed.
        ///
        /// Mirrors `RendererTextures.cpp`'s own `texture_data_bytes` exactly, including its
        /// overflow checks and format coverage -- the two must agree on every format either one of
        /// them is ever asked to pack, since `upload_texture_rgba` and `TextureStreamer` share this
        /// implementation for that reason.
        ///
        /// @param format Texture format.
        /// @param width Pixel width of the level.
        /// @param height Pixel height of the level.
        ///
        /// @return Returns the value produced by the operation, or 0 for a format with no known size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 texture_data_bytes(RHI::Format format, u32 width, u32 height) noexcept {
            if (width == 0 || height == 0) {
                return 0;
            }
            if (RHI::format_is_block_compressed(format)) {
                u32 bytes_per_block = 0;
                switch (format) {
                    case RHI::Format::BC7Unorm:
                    case RHI::Format::BC7UnormSrgb:
                    case RHI::Format::BC5Unorm:
                    case RHI::Format::BC3Unorm:
                    case RHI::Format::BC3UnormSrgb: bytes_per_block = 16; break;
                    case RHI::Format::BC4Unorm:
                    case RHI::Format::BC1Unorm:
                    case RHI::Format::BC1UnormSrgb: bytes_per_block = 8; break;
                    default: return 0;
                }
                const u64 blocks_wide = (static_cast<u64>(width) + 3u) / 4u;
                const u64 blocks_high = (static_cast<u64>(height) + 3u) / 4u;
                if (blocks_wide > std::numeric_limits<u64>::max() / blocks_high) {
                    return 0;
                }
                const u64 block_count = blocks_wide * blocks_high;
                return block_count <= std::numeric_limits<u64>::max() / bytes_per_block
                    ? block_count * bytes_per_block
                    : 0;
            }

            u32 texel_size = 0;
            switch (format) {
                case RHI::Format::R8Unorm: texel_size = 1; break;
                case RHI::Format::RGBA8Unorm:
                case RHI::Format::RGBA8UnormSrgb: texel_size = 4; break;
                // The format an HDR source texture (an EXR environment map, a PQ AVIF) is uploaded
                // in: no 8-bit format can hold scene-linear light above display white, and no BC
                // format this engine can encode holds float at all.
                case RHI::Format::RGBA16Float: texel_size = 8; break;
                default: return 0;
            }
            const u64 texels = static_cast<u64>(width) * height;
            return texels <= std::numeric_limits<u64>::max() / texel_size ? texels * texel_size : 0;
        }

        /// Rounds `value` up to a multiple of `alignment`.
        ///
        /// @param value Value consumed by the operation.
        /// @param alignment Alignment to round up to.
        ///
        /// @return Returns the value produced by the operation, or 0 on overflow.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 align_up(u64 value, u64 alignment) noexcept {
            if (alignment == 0 || value > std::numeric_limits<u64>::max() - (alignment - 1u)) {
                return 0;
            }
            return ((value + alignment - 1u) / alignment) * alignment;
        }

    } // namespace

    /// Reports whether `backend` requires each row of a buffer-to-texture copy to start on a
    /// 256-byte boundary.
    ///
    /// @param backend Backend the copy will run against.
    ///
    /// @return Returns `true` when rows must be padded before uploading.
    /// @note This function does not throw exceptions.
    bool backend_requires_padded_texture_rows(RHI::BackendType backend) noexcept {
        return backend == RHI::BackendType::D3D12 || backend == RHI::BackendType::WebGpu;
    }

    /// Repacks a tightly packed mip chain so each level's rows start on a 256-byte boundary and
    /// each level itself starts on a 512-byte boundary.
    ///
    /// @param format Pixel format the data is encoded as.
    /// @param width Level-0 width.
    /// @param height Level-0 height.
    /// @param mip_levels Number of mip levels present in `tight_data`, tightly packed in order.
    /// @param tight_data The whole mip chain, tightly packed.
    ///
    /// @return Returns the repacked buffer.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<std::byte> pack_texture_upload_rows(RHI::Format format, u32 width, u32 height, u32 mip_levels,
                                               span<const std::byte> tight_data) {
        vector<std::byte> padded;
        u64 source_offset = 0;
        u64 destination_offset = 0;
        u32 level_width = width;
        u32 level_height = height;
        for (u32 level = 0; level < mip_levels; ++level) {
            const u64 tight_row_bytes = texture_data_bytes(format, level_width, 1);
            const u64 row_pitch = align_up(tight_row_bytes, 256);
            destination_offset = align_up(destination_offset, 512);
            const u32 stored_rows = texture_row_count(format, level_height);
            const u64 required = destination_offset + row_pitch * stored_rows;
            if (required > padded.size()) {
                padded.resize(static_cast<usize>(required));
            }
            for (u32 row = 0; row < stored_rows; ++row) {
                if (source_offset + static_cast<u64>(row) * tight_row_bytes + tight_row_bytes >
                    tight_data.size()) {
                    // The caller's mip count or dimensions do not match the data it gave us; stop
                    // rather than read past the end of it.
                    break;
                }
                std::memcpy(padded.data() + destination_offset + static_cast<u64>(row) * row_pitch,
                            tight_data.data() + source_offset + static_cast<u64>(row) * tight_row_bytes,
                            static_cast<usize>(tight_row_bytes));
            }
            source_offset += texture_data_bytes(format, level_width, level_height);
            destination_offset = required;
            level_width = std::max(level_width / 2u, 1u);
            level_height = std::max(level_height / 2u, 1u);
        }
        return padded;
    }

} // namespace SFT::Renderer
