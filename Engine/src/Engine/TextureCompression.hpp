#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::RHI {

    /// Forward-declared rather than #including RHI/RHI.hpp — only the type is needed here.
    /// Underlying type must match RHI/src/RHI/Types.hpp's real definition exactly.
    enum class Format : u32;

} // namespace SFT::RHI

namespace SFT::Engine {

    /// Forward-declared rather than #including AssetManager.hpp — Detail::choose_bc_format below only
    /// needs the enum type, and AssetManager.hpp has no reason to depend on this header. Underlying
    /// type must match the real definition in AssetManager.hpp exactly.
    enum class TextureKind : u8;

} // namespace SFT::Engine

namespace SFT::Engine::Detail {

    /// BC7-encodes `rgba8` (tightly-packed width*height*4 bytes, as validated by
    /// AssetManager::create_texture) for a real VRAM win (~4x smaller than RGBA8Unorm) with no
    /// shader changes anywhere (BC7 stores full RGBA, so it's a drop-in replacement for
    /// RGBA8Unorm/RGBA8UnormSrgb wherever a texture is sampled). Checks/populates an on-disk cache
    /// under .cache/compressed_textures/ first (keyed by a content hash of the pixels + dimensions
    /// + `srgb`) so repeat loads of the same texture skip bc7enc's real encode cost, which is
    /// CPU-slow at the quality level this uses.
    ///
    /// Returns nullopt (never a hard error) for invalid input or if encoding fails for any reason —
    /// callers fall back to uploading `rgba8` uncompressed in either case, exactly like a device that
    /// doesn't report RHI::DeviceLimits::supports_bc_texture_compression at all. Sub-4x4 mip levels
    /// are valid: their edge texels are replicated to fill the final BC7 block.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);

    /// BC7-encodes a tightly packed RGBA8 mip chain (largest to smallest) one level at a time and
    /// returns the correspondingly packed BC7 chain. Level dimensions are derived by halving each
    /// previous dimension with a floor of one.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);

    /// BC1-encodes `rgba8` — opaque RGB only (alpha is ignored on encode and always reads back as
    /// 1.0), half BC7's size. Only safe for textures where alpha genuinely doesn't matter (see
    /// Engine::TextureKind::ColorOpaque / Engine::AssetManager::create_texture's kind-aware policy).
    /// Same disk-cache-first, "returns nullopt, never a hard error" contract as compress_bc7 (cache
    /// files under the same .cache/compressed_textures/ directory, .sbc1 extension).
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc1(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc1_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);

    /// BC3-encodes `rgba8` — full RGBA with smoothly interpolated (not endpoint-exact) alpha, same
    /// size as BC7 but a cheaper/lower-quality encode. Same contract as compress_bc7 (.sbc3 cache).
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc3(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc3_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, bool srgb);

    /// BC4-encodes a single channel (`channel`, 0-3) of `rgba8` — half BC7's size, for a standalone
    /// single-channel mask/data texture (e.g. an occlusion map with no metallic-roughness to pack
    /// with — see Engine::AssetManager::create_orm_texture for the packed case). Decodes back to
    /// (R, 0, 0, 1) regardless of API, so only safe for a texture a shader samples via `.r` alone.
    /// `srgb` has no bearing on single-channel data but is still part of the cache key/header shape
    /// for symmetry with the other compress_bc*() entry points. Same contract otherwise (.sbc4 cache).
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc4(
        std::span<const std::byte> rgba8, u32 width, u32 height, u32 channel = 0);
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc4_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels, u32 channel = 0);

    /// BC5-encodes two channels (`channel0`/`channel1`, default R/G) of `rgba8` — same size as BC7,
    /// for 2-channel data such as a tangent-space normal map (X/Y only — the shader reconstructs Z,
    /// see Shaders/gbuffer_geometry.slang). Decodes back to (R, G, 0, 1), so only safe for a texture
    /// a shader samples via `.rg` alone. Same contract otherwise (.sbc5 cache).
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc5(
        std::span<const std::byte> rgba8, u32 width, u32 height, u32 channel0 = 0, u32 channel1 = 1);
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc5_mip_chain(
        std::span<const std::byte> rgba8_mips, u32 width, u32 height, u32 mip_levels,
        u32 channel0 = 0, u32 channel1 = 1);

    /// GDeflate-compresses `bc7_blocks` (compress_bc7's own output) for the texture-streaming asset
    /// pipeline (Engine::TextureStreamer's GDeflate pipeline stage, Core::decompress_gdeflate on the
    /// read side): a real shipped/streamed texture asset can carry this smaller representation instead
    /// of raw BC7 blocks. Same disk-content-hash-cached shape as compress_bc7 (keyed off `bc7_blocks`
    /// itself, not the original RGBA8 source, so a cache hit needs only the BC7 bytes already in hand
    /// -- not the decoded image that produced them), same "returns nullopt, never a hard error"
    /// fallback contract on any GDeflate failure.
    ///
    /// Scope note: this is the WRITE/cache side only, validating the container shape end-to-end
    /// (compress -> disk cache -> Core::decompress_gdeflate round trip, see
    /// EngineTextureCompressionGDeflateTest). Wiring Engine::TextureStreamer to actually read a
    /// pre-baked GDeflate-compressed asset from disk (instead of decoding a PNG/JPEG at request time)
    /// needs a real offline asset-baking/import pipeline this engine does not have yet -- that's
    /// future work, not part of this pass.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_gdeflate_sibling(
        std::span<const std::byte> bc7_blocks, u32 width, u32 height, bool srgb);

    /// The "Compression Manager" policy: which BC format a texture of a given semantic kind should
    /// use. Kept separate from the compress_bc*() encoders above (mechanism) so the kind -> format
    /// decision table lives in one obvious place. `Format::Undefined` is never returned.
    [[nodiscard]] RHI::Format choose_bc_format(TextureKind kind, bool srgb) noexcept;

    /// Builds one packed RGBA8 texture from a standalone occlusion map (R channel) and a glTF-style
    /// metallic-roughness map (G = roughness, B = metallic) so both can be bound as a single GPU
    /// texture instead of two — see AssetManager::create_orm_texture. Both inputs must be
    /// tightly-packed width*height*4 RGBA8 buffers of identical `width`/`height`; returns nullopt
    /// otherwise (callers fall back to uploading the two source textures independently rather than
    /// risk misaligning channels from mismatched sources).
    [[nodiscard]] std::optional<std::vector<std::byte>> pack_orm_rgba8(
        std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
        u32 width, u32 height);

    /// Re-channels a standalone glTF-style metallic-roughness map (G = roughness, B = metallic) into
    /// R = roughness, G = metallic, so it can go through compress_bc5's default R/G channels — see
    /// Engine::TextureKind::MetallicRoughness. Done as an explicit CPU-side repack (rather than
    /// asking compress_bc5 to read channels 1/2 directly from the original layout) so the *uploaded*
    /// pixel data is correct even when compression is skipped entirely (unsupported device, or a
    /// sub-4x4 texture) — the caller can set its shader-side "which channels" flag unconditionally
    /// once, instead of depending on whether compression actually happened.
    [[nodiscard]] std::optional<std::vector<std::byte>> pack_metallic_roughness_rg(
        std::span<const std::byte> metallic_roughness_rgba8, u32 width, u32 height);

} // namespace SFT::Engine::Detail
