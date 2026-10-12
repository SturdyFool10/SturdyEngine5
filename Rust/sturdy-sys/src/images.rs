//! Raw `cxx` bindings for the engine's CPU-side image pipeline (`Engine/ImageDecode.hpp`,
//! `TextureMipChain.hpp`, `TextureCompression.hpp`): decoding any supported image format,
//! generating mip chains, and block-compressing (BC1/3/4/5/7) — all standalone functions with no
//! `Engine` dependency, so nothing here takes an `EngineView`.
//!
//! Every function flattens the C++ `std::optional`/`std::expected` into an `ok`/`error` result
//! struct so only plain data crosses the boundary.

#[cxx::bridge(namespace = "sturdy_rs::images")]
pub mod ffi {
    /// Mirrors `SFT::Engine::Detail::DecodePrecision`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum DecodePrecision {
        Rgba8Srgb,
        SceneLinear,
        Native,
    }

    /// Mirrors `SFT::Engine::Detail::GamutMapping`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum GamutMapping {
        Clip,
        Desaturate,
    }

    /// Mirrors `SFT::Engine::Detail::PixelFormat` (always four-channel, tightly packed).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum PixelFormat {
        Rgba8,
        Rgba16,
        Rgba16F,
        Rgba32F,
    }

    /// Which block-compression codec [`compress_bc`] runs.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum BcCodec {
        Bc1,
        Bc3,
        Bc4,
        Bc5,
        Bc7,
    }

    #[derive(Debug, Clone, Default)]
    struct DecodedFrame {
        pixels: Vec<u8>,
        duration_ms: u32,
    }

    #[derive(Debug, Clone)]
    struct DecodeResult {
        ok: bool,
        /// Populated when `!ok`.
        error: String,
        width: u32,
        height: u32,
        format: PixelFormat,
        color_primaries: u8,
        transfer_function: u8,
        frames: Vec<DecodedFrame>,
        loop_count: u32,
        is_preview: bool,
        full_width: u32,
        full_height: u32,
        hdr_present: bool,
        hdr_color_primaries: u8,
        hdr_transfer_function: u8,
        hdr_max_content_light_level: u32,
        hdr_max_frame_average_light_level: u32,
    }

    /// Result of every byte-producing operation (compression, packing). `ok == false` is the C++
    /// `std::nullopt` (bad dimensions, undersized input, ...).
    #[derive(Debug, Clone, Default)]
    struct BytesResult {
        ok: bool,
        bytes: Vec<u8>,
    }

    #[derive(Debug, Clone, Default)]
    struct LicenseEntry {
        project: String,
        license_file: String,
        text: String,
    }

    #[derive(Debug, Clone, Default)]
    struct MipChainResult {
        ok: bool,
        bytes: Vec<u8>,
        mip_levels: u32,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/images.hpp");

        /// `source` is only used for error messages.
        fn decode_image(
            encoded: &[u8],
            precision: DecodePrecision,
            gamut_mapping: GamutMapping,
            decode_all_frames: bool,
            preview_min_dimension: u32,
            source: &str,
        ) -> DecodeResult;

        fn texture_mip_level_count(width: u32, height: u32) -> u32;
        fn generate_rgba8_mip_chain(rgba8: &[u8], width: u32, height: u32, srgb: bool) -> MipChainResult;
        fn generate_rgba16f_mip_chain(rgba16f: &[u8], width: u32, height: u32) -> MipChainResult;

        /// Compresses `rgba8` (one level when `mip_levels <= 1`, else that many tightly packed
        /// levels). `srgb` applies to BC1/3/7; `channel0`/`channel1` to BC4 (channel0) / BC5.
        fn compress_bc(
            codec: BcCodec,
            rgba8: &[u8],
            width: u32,
            height: u32,
            mip_levels: u32,
            srgb: bool,
            channel0: u32,
            channel1: u32,
        ) -> BytesResult;

        fn compress_gdeflate_sibling(bc7_blocks: &[u8], width: u32, height: u32, srgb: bool) -> BytesResult;

        /// `kind` is the numeric value of `assets::TextureKind`; returns the numeric value of
        /// `rhi::Format`.
        fn choose_bc_format(kind: u8, srgb: bool) -> u8;

        /// GDeflate (GPU-decompressible) compression of arbitrary bytes at `level` (1..=12).
        fn gdeflate_compress(data: &[u8], level: u32) -> BytesResult;
        /// Decompresses GDeflate data; `decompressed_size` must be the original length.
        fn gdeflate_decompress(compressed: &[u8], decompressed_size: u64) -> BytesResult;
        /// Reads a whole file with the fastest path the platform offers (io_uring / DirectStorage).
        fn read_file_accelerated(path: &str) -> BytesResult;
        /// The third-party projects the engine embeds, with their license texts.
        fn third_party_licenses() -> Vec<LicenseEntry>;
        fn pack_orm_rgba8(occlusion_rgba8: &[u8], metallic_roughness_rgba8: &[u8], width: u32, height: u32) -> BytesResult;
        fn pack_metallic_roughness_rg(metallic_roughness_rgba8: &[u8], width: u32, height: u32) -> BytesResult;
    }
}
