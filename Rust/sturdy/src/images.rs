//! CPU-side image pipeline: decoding any format the engine supports, mip-chain generation, and
//! BC1/3/4/5/7 block compression. Standalone functions — no [`crate::Engine`] needed, so they are
//! usable from tools and worker threads.
//!
//! Buffers are plain tightly-packed `&[u8]`/`Vec<u8>`; mip chains and compressed chains are all
//! levels concatenated, largest first.

use sturdy_sys::images::ffi as sys;

use crate::assets::TextureKind;
use crate::rhi::Format;

/// Destination color handling for [`decode`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum DecodePrecision {
    /// Display-ready 8-bit sRGB (`PixelFormat::Rgba8`), color-managed from wide-gamut/HDR sources.
    #[default]
    Rgba8Srgb,
    /// Linear light in BT.709 primaries at half-float or wider; nothing clamped to display range.
    SceneLinear,
    /// Exactly what the file carries, with no color conversion.
    Native,
}

/// How out-of-gamut colors are handled when converting a wide-gamut source.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum GamutMapping {
    Clip,
    #[default]
    Desaturate,
}

/// Sample layout of a decoded frame; always four-channel, tightly packed, top-to-bottom.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    Rgba8,
    Rgba16,
    Rgba16F,
    Rgba32F,
}

impl PixelFormat {
    pub fn bytes_per_pixel(self) -> u32 {
        match self {
            Self::Rgba8 => 4,
            Self::Rgba16 | Self::Rgba16F => 8,
            Self::Rgba32F => 16,
        }
    }
}

/// Knobs for [`decode`]. The defaults: first frame only, 8-bit sRGB, full resolution.
#[derive(Debug, Clone, Copy, Default)]
pub struct DecodeOptions {
    pub precision: DecodePrecision,
    pub gamut_mapping: GamutMapping,
    /// Decode every frame of an animated source instead of just the first.
    pub decode_all_frames: bool,
    /// When non-zero, request a cheap reduced-detail decode at least this many pixels on both
    /// axes, if the codec supports it (see [`DecodedImage::is_preview`]).
    pub preview_min_dimension: u32,
}

/// PNG `cICP`/`cLLI` HDR metadata. Code points are raw ITU-T H.273 values.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PngHdrMetadata {
    pub color_primaries: u8,
    pub transfer_function: u8,
    /// cd/m²; zero when the chunk was absent.
    pub max_content_light_level: u32,
    pub max_frame_average_light_level: u32,
}

#[derive(Debug, Clone)]
pub struct ImageFrame {
    pub pixels: Vec<u8>,
    /// Milliseconds this frame is shown; zero for stills.
    pub duration_ms: u32,
}

#[derive(Debug, Clone)]
pub struct DecodedImage {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    /// ITU-T H.273 code points describing what the samples mean.
    pub color_primaries: u8,
    pub transfer_function: u8,
    /// Never empty.
    pub frames: Vec<ImageFrame>,
    /// Zero means loop forever (and is the right answer for a still).
    pub loop_count: u32,
    /// True when a reduced-detail decode happened; `width`/`height` are then the preview's size.
    pub is_preview: bool,
    pub full_width: u32,
    pub full_height: u32,
    /// Only ever `Some` for a PNG carrying HDR chunks.
    pub hdr: Option<PngHdrMetadata>,
}

impl DecodedImage {
    /// The first frame's pixels — the whole image for a still.
    pub fn pixels(&self) -> &[u8] {
        &self.frames[0].pixels
    }
}

/// Why an image operation failed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ImageError {
    /// The decoder rejected the data; carries its message.
    Decode(String),
    /// The operation produced nothing: zero/invalid dimensions or an undersized input buffer.
    InvalidInput,
}

impl core::fmt::Display for ImageError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Decode(message) => write!(f, "image decode failed: {message}"),
            Self::InvalidInput => f.write_str("invalid image dimensions or undersized pixel buffer"),
        }
    }
}

impl std::error::Error for ImageError {}

/// Decodes an encoded image. `source` is only used in error messages (e.g. the file name).
pub fn decode(encoded: &[u8], options: DecodeOptions, source: &str) -> Result<DecodedImage, ImageError> {
    let precision = match options.precision {
        DecodePrecision::Rgba8Srgb => sys::DecodePrecision::Rgba8Srgb,
        DecodePrecision::SceneLinear => sys::DecodePrecision::SceneLinear,
        DecodePrecision::Native => sys::DecodePrecision::Native,
    };
    let gamut = match options.gamut_mapping {
        GamutMapping::Clip => sys::GamutMapping::Clip,
        GamutMapping::Desaturate => sys::GamutMapping::Desaturate,
    };
    let r = sys::decode_image(
        encoded,
        precision,
        gamut,
        options.decode_all_frames,
        options.preview_min_dimension,
        source,
    );
    if !r.ok {
        return Err(ImageError::Decode(r.error));
    }
    let format = match r.format {
        sys::PixelFormat::Rgba16 => PixelFormat::Rgba16,
        sys::PixelFormat::Rgba16F => PixelFormat::Rgba16F,
        sys::PixelFormat::Rgba32F => PixelFormat::Rgba32F,
        _ => PixelFormat::Rgba8,
    };
    Ok(DecodedImage {
        width: r.width,
        height: r.height,
        format,
        color_primaries: r.color_primaries,
        transfer_function: r.transfer_function,
        frames: r
            .frames
            .into_iter()
            .map(|f| ImageFrame { pixels: f.pixels, duration_ms: f.duration_ms })
            .collect(),
        loop_count: r.loop_count,
        is_preview: r.is_preview,
        full_width: r.full_width,
        full_height: r.full_height,
        hdr: r.hdr_present.then_some(PngHdrMetadata {
            color_primaries: r.hdr_color_primaries,
            transfer_function: r.hdr_transfer_function,
            max_content_light_level: r.hdr_max_content_light_level,
            max_frame_average_light_level: r.hdr_max_frame_average_light_level,
        }),
    })
}

/// A full mip chain: every level tightly packed and concatenated, largest first.
#[derive(Debug, Clone)]
pub struct MipChain {
    pub data: Vec<u8>,
    pub mip_levels: u32,
}

/// Number of mip levels for a `width`×`height` texture.
pub fn mip_level_count(width: u32, height: u32) -> u32 {
    sys::texture_mip_level_count(width, height)
}

fn chain(r: sys::MipChainResult) -> Result<MipChain, ImageError> {
    if r.ok { Ok(MipChain { data: r.bytes, mip_levels: r.mip_levels }) } else { Err(ImageError::InvalidInput) }
}

fn bytes(r: sys::BytesResult) -> Result<Vec<u8>, ImageError> {
    if r.ok { Ok(r.bytes) } else { Err(ImageError::InvalidInput) }
}

/// Area-averaged mip chain for tightly packed RGBA8. With `srgb`, averaging happens in linear light.
pub fn generate_rgba8_mip_chain(rgba8: &[u8], width: u32, height: u32, srgb: bool) -> Result<MipChain, ImageError> {
    chain(sys::generate_rgba8_mip_chain(rgba8, width, height, srgb))
}

/// Mip chain for scene-linear tightly packed half-float RGBA; nothing is clamped.
pub fn generate_rgba16f_mip_chain(rgba16f: &[u8], width: u32, height: u32) -> Result<MipChain, ImageError> {
    chain(sys::generate_rgba16f_mip_chain(rgba16f, width, height))
}

/// A block-compression codec.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BcCodec {
    /// Opaque color, 4 bpp.
    Bc1 { srgb: bool },
    /// Color + separate-alpha blocks, 8 bpp.
    Bc3 { srgb: bool },
    /// One channel (0 = R .. 3 = A), 4 bpp.
    Bc4 { channel: u32 },
    /// Two channels (e.g. normal map XY), 8 bpp.
    Bc5 { channel0: u32, channel1: u32 },
    /// High-quality color + alpha, 8 bpp.
    Bc7 { srgb: bool },
}

/// Compresses one level of tightly packed RGBA8.
pub fn compress(codec: BcCodec, rgba8: &[u8], width: u32, height: u32) -> Result<Vec<u8>, ImageError> {
    compress_mip_chain(codec, rgba8, width, height, 1)
}

/// Compresses `mip_levels` concatenated RGBA8 levels (as produced by
/// [`generate_rgba8_mip_chain`]) into concatenated compressed levels.
pub fn compress_mip_chain(
    codec: BcCodec,
    rgba8: &[u8],
    width: u32,
    height: u32,
    mip_levels: u32,
) -> Result<Vec<u8>, ImageError> {
    let (c, srgb, ch0, ch1) = match codec {
        BcCodec::Bc1 { srgb } => (sys::BcCodec::Bc1, srgb, 0, 1),
        BcCodec::Bc3 { srgb } => (sys::BcCodec::Bc3, srgb, 0, 1),
        BcCodec::Bc4 { channel } => (sys::BcCodec::Bc4, false, channel, 1),
        BcCodec::Bc5 { channel0, channel1 } => (sys::BcCodec::Bc5, false, channel0, channel1),
        BcCodec::Bc7 { srgb } => (sys::BcCodec::Bc7, srgb, 0, 1),
    };
    bytes(sys::compress_bc(c, rgba8, width, height, mip_levels, srgb, ch0, ch1))
}

/// GDeflate-compresses BC7 blocks for GPU-side decompression streaming.
pub fn compress_gdeflate_sibling(bc7_blocks: &[u8], width: u32, height: u32, srgb: bool) -> Result<Vec<u8>, ImageError> {
    bytes(sys::compress_gdeflate_sibling(bc7_blocks, width, height, srgb))
}

/// A third-party project bundled in the engine and its license.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct License {
    pub project: String,
    pub license_file: String,
    pub text: String,
}

/// The licenses of everything the engine embeds (show them in an "about" screen).
pub fn third_party_licenses() -> Vec<License> {
    sys::third_party_licenses()
        .into_iter()
        .map(|l| License { project: l.project, license_file: l.license_file, text: l.text })
        .collect()
}

/// GDeflate-compresses `data` (GPU-decompressible container) at `level` 1..=12.
pub fn gdeflate_compress(data: &[u8], level: u32) -> Result<Vec<u8>, ImageError> {
    bytes(sys::gdeflate_compress(data, level))
}

/// Decompresses [`gdeflate_compress`] output; `decompressed_size` is the original length.
pub fn gdeflate_decompress(compressed: &[u8], decompressed_size: usize) -> Result<Vec<u8>, ImageError> {
    bytes(sys::gdeflate_decompress(compressed, decompressed_size as u64))
}

/// Reads a whole file via the platform's fastest path (io_uring, DirectStorage); `None` if it fails.
pub fn read_file_accelerated(path: impl AsRef<std::path::Path>) -> Option<Vec<u8>> {
    let r = sys::read_file_accelerated(&path.as_ref().to_string_lossy());
    r.ok.then_some(r.bytes)
}

/// The BC format the engine picks for a texture of `kind`.
pub fn choose_bc_format(kind: TextureKind, srgb: bool) -> Format {
    let kind = match kind {
        TextureKind::ColorAlpha => 0,
        TextureKind::ColorOpaque => 1,
        TextureKind::Mask => 2,
        TextureKind::NormalMap => 3,
        TextureKind::MetallicRoughness => 4,
    };
    Format { repr: sys::choose_bc_format(kind, srgb) }
}

/// Packs separate occlusion and metallic-roughness RGBA8 images into one ORM texture.
pub fn pack_orm_rgba8(occlusion_rgba8: &[u8], metallic_roughness_rgba8: &[u8], width: u32, height: u32) -> Result<Vec<u8>, ImageError> {
    bytes(sys::pack_orm_rgba8(occlusion_rgba8, metallic_roughness_rgba8, width, height))
}

/// Repacks a glTF metallic-roughness RGBA8 image (roughness in G, metallic in B) so those two
/// channels land in R and G of the result (B = 0, A = 255), ready for BC5 compression.
pub fn pack_metallic_roughness_rg(metallic_roughness_rgba8: &[u8], width: u32, height: u32) -> Result<Vec<u8>, ImageError> {
    bytes(sys::pack_metallic_roughness_rg(metallic_roughness_rgba8, width, height))
}
