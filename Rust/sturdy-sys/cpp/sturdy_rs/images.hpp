// Rust <-> SturdyEngine 5 shim for the CPU-side image pipeline (decode, mip chains, BC
// compression). See src/images.rs. Standalone functions; no engine instance involved.
#pragma once

#include <cstdint>

#include <rust/cxx.h>

namespace sturdy_rs::images {

    enum class DecodePrecision : std::uint8_t;
    enum class GamutMapping : std::uint8_t;
    enum class PixelFormat : std::uint8_t;
    enum class BcCodec : std::uint8_t;
    struct DecodeResult;
    struct BytesResult;
    struct MipChainResult;
    struct LicenseEntry;

    DecodeResult decode_image(rust::Slice<const std::uint8_t> encoded, DecodePrecision precision,
                              GamutMapping gamut_mapping, bool decode_all_frames,
                              std::uint32_t preview_min_dimension, rust::Str source);

    std::uint32_t texture_mip_level_count(std::uint32_t width, std::uint32_t height);
    MipChainResult generate_rgba8_mip_chain(rust::Slice<const std::uint8_t> rgba8, std::uint32_t width,
                                            std::uint32_t height, bool srgb);
    MipChainResult generate_rgba16f_mip_chain(rust::Slice<const std::uint8_t> rgba16f,
                                              std::uint32_t width, std::uint32_t height);

    BytesResult compress_bc(BcCodec codec, rust::Slice<const std::uint8_t> rgba8, std::uint32_t width,
                            std::uint32_t height, std::uint32_t mip_levels, bool srgb,
                            std::uint32_t channel0, std::uint32_t channel1);
    BytesResult compress_gdeflate_sibling(rust::Slice<const std::uint8_t> bc7_blocks,
                                          std::uint32_t width, std::uint32_t height, bool srgb);
    std::uint8_t choose_bc_format(std::uint8_t kind, bool srgb);

    BytesResult gdeflate_compress(rust::Slice<const std::uint8_t> data, std::uint32_t level);
    BytesResult gdeflate_decompress(rust::Slice<const std::uint8_t> compressed, std::uint64_t decompressed_size);
    BytesResult read_file_accelerated(rust::Str path);
    rust::Vec<LicenseEntry> third_party_licenses();

    BytesResult pack_orm_rgba8(rust::Slice<const std::uint8_t> occlusion_rgba8,
                               rust::Slice<const std::uint8_t> metallic_roughness_rgba8,
                               std::uint32_t width, std::uint32_t height);
    BytesResult pack_metallic_roughness_rg(rust::Slice<const std::uint8_t> metallic_roughness_rgba8,
                                           std::uint32_t width, std::uint32_t height);

} // namespace sturdy_rs::images
