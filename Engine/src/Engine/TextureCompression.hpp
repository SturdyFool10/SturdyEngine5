#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    // BC7-encodes `rgba8` (tightly-packed width*height*4 bytes, as validated by
    // AssetManager::create_texture) for a real VRAM win (~4x smaller than RGBA8Unorm) with no
    // shader changes anywhere (BC7 stores full RGBA, so it's a drop-in replacement for
    // RGBA8Unorm/RGBA8UnormSrgb wherever a texture is sampled). Checks/populates an on-disk cache
    // under .cache/compressed_textures/ first (keyed by a content hash of the pixels + dimensions
    // + `srgb`) so repeat loads of the same texture skip bc7enc's real encode cost, which is
    // CPU-slow at the quality level this uses.
    //
    // Returns nullopt (never a hard error) for invalid input or if encoding fails for any reason —
    // callers fall back to uploading `rgba8` uncompressed in either case, exactly like a device that
    // doesn't report RHI::DeviceLimits::supports_bc_texture_compression at all. Sub-4x4 mip levels
    // are valid: their edge texels are replicated to fill the final BC7 block.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);

    // BC7-encodes a tightly packed RGBA8 mip chain (largest to smallest) one level at a time and
    // returns the correspondingly packed BC7 chain. Level dimensions are derived by halving each
    // previous dimension with a floor of one.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);

    // GDeflate-compresses `bc7_blocks` (compress_bc7's own output) for the texture-streaming asset
    // pipeline (Engine::TextureStreamer's GDeflate pipeline stage, Core::decompress_gdeflate on the
    // read side): a real shipped/streamed texture asset can carry this smaller representation instead
    // of raw BC7 blocks. Same disk-content-hash-cached shape as compress_bc7 (keyed off `bc7_blocks`
    // itself, not the original RGBA8 source, so a cache hit needs only the BC7 bytes already in hand
    // -- not the decoded image that produced them), same "returns nullopt, never a hard error"
    // fallback contract on any GDeflate failure.
    //
    // Scope note: this is the WRITE/cache side only, validating the container shape end-to-end
    // (compress -> disk cache -> Core::decompress_gdeflate round trip, see
    // EngineTextureCompressionGDeflateTest). Wiring Engine::TextureStreamer to actually read a
    // pre-baked GDeflate-compressed asset from disk (instead of decoding a PNG/JPEG at request time)
    // needs a real offline asset-baking/import pipeline this engine does not have yet -- that's
    // future work, not part of this pass.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_gdeflate_sibling(
        std::span<const std::byte> bc7_blocks, u32 width, u32 height, bool srgb);

} // namespace SFT::Engine::Detail
