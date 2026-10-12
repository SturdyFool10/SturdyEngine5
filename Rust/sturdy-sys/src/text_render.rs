//! Raw `cxx` bindings for GPU text rendering: the engine's glyph atlas + text pipeline
//! (`TextAtlas`, `TextPipeline`) and `TextRenderTarget`, wrapped in one `TextRenderer` object that
//! renders shaped glyphs into offscreen textures.
//!
//! A render target is an ordinary RHI texture (sampled, color attachment) plus a sampler, so its
//! handles drop straight into your own RHI bind groups/shaders; it can also be adopted as a
//! Renderer texture (for UI images) or read back to the CPU (for asset textures, e.g. world text).
//! Rendering is synchronous: each `text_target_render` submits and waits.
//!
//! Glyph placements carry a baseline pen position in pixels from the target's top-left (y down).

#[cxx::bridge(namespace = "sturdy_rs::text_render")]
pub mod ffi {
    /// One glyph to draw. `font_id` is a `sturdy_sys::text` registry id.
    #[derive(Debug, Clone, Copy)]
    struct GlyphPlacementDesc {
        /// Baseline pen position (pixels from the target's top-left).
        x: f32,
        y: f32,
        rotation: f32,
        color: [f32; 4],
        font_id: u64,
        glyph_id: u32,
        units_per_em: u32,
        /// The em size in pixels the glyph is drawn at.
        pixel_size: f32,
        is_color: bool,
        stem_darkening: bool,
    }

    #[derive(Debug, Clone, Copy)]
    struct AtlasConfigDesc {
        initial_image_size: u32,
        maximum_image_size: u32,
        pixel_range: f32,
        padding_px: f32,
    }

    /// The RHI resources behind a text target. Handles are raw `rhi` handle values.
    #[derive(Debug, Clone, Copy, Default)]
    struct TargetInfo {
        ok: bool,
        id: u64,
        texture: u64,
        view: u64,
        sampler: u64,
        width: u32,
        height: u32,
        /// `rhi::Format` value.
        format: u8,
    }

    #[derive(Debug, Clone)]
    struct TargetResult {
        info: TargetInfo,
        error: String,
    }

    #[derive(Debug, Clone, Default)]
    struct StatusResult {
        ok: bool,
        error: String,
    }

    #[derive(Debug, Clone, Default)]
    struct PixelsResult {
        ok: bool,
        error: String,
        width: u32,
        height: u32,
        bytes_per_pixel: u32,
        pixels: Vec<u8>,
    }

    /// A resident tile of a text canvas: an RGBA8 texture covering `logical_*` (canvas pixels).
    #[derive(Debug, Clone, Copy, Default)]
    struct CanvasTile {
        tile_x: i32,
        tile_y: i32,
        texture: u64,
        view: u64,
        logical_x: i32,
        logical_y: i32,
        logical_width: u32,
        logical_height: u32,
    }

    #[derive(Debug, Clone)]
    struct CanvasResult {
        ok: bool,
        error: String,
        id: u64,
        tile_size: u32,
    }

    #[derive(Debug, Clone, Default)]
    struct CanvasTilesResult {
        ok: bool,
        error: String,
        tiles: Vec<CanvasTile>,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/text_render.hpp");

        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;

        /// Owns the glyph atlas, text pipelines and render targets. Must be destroyed (dropped or
        /// `text_renderer_destroy`ed) while the engine's GPU device is alive.
        type TextRendererBox;

        /// Null when the engine has no GPU device yet (call from a frame, not `init`).
        fn text_renderer_new(engine: Pin<&mut EngineView>, config: &AtlasConfigDesc) -> UniquePtr<TextRendererBox>;
        fn text_renderer_destroy(renderer: Pin<&mut TextRendererBox>);

        /// Creates an offscreen target of an `rhi::Format` that can be a color attachment.
        fn text_target_create(renderer: Pin<&mut TextRendererBox>, width: u32, height: u32, format: u8) -> TargetResult;
        fn text_target_info(renderer: &TextRendererBox, id: u64) -> TargetInfo;
        /// Clears the target to transparent and draws `glyphs` into it.
        fn text_target_render(renderer: Pin<&mut TextRendererBox>, id: u64, glyphs: &[GlyphPlacementDesc]) -> StatusResult;
        /// Copies the target's pixels to the CPU (tight rows, top to bottom).
        fn text_target_read(renderer: Pin<&mut TextRendererBox>, id: u64) -> PixelsResult;
        /// Registers the target as a Renderer texture (for UI images); returns the raw
        /// `assets::RendererTextureHandle`, or 0. Valid until the target is destroyed.
        fn text_target_adopt(renderer: &TextRendererBox, engine: Pin<&mut EngineView>, id: u64) -> u64;
        fn text_target_destroy(renderer: Pin<&mut TextRendererBox>, id: u64) -> bool;

        // -- Text canvas: a huge virtual surface backed by lazily rendered tiles ------------------

        /// Creates a canvas whose tiles are `desired_tile_size` px squares (clamped to the device
        /// limit); at most `max_resident_tiles` stay in GPU memory (least recently used are evicted).
        fn text_canvas_create(renderer: Pin<&mut TextRendererBox>, desired_tile_size: u32, max_resident_tiles: u32) -> CanvasResult;
        /// Adds glyphs at canvas coordinates (any i32 position, y down). Tiles they touch that are
        /// already resident are re-rendered on next use.
        fn text_canvas_draw(renderer: Pin<&mut TextRendererBox>, id: u64, glyphs: &[GlyphPlacementDesc]) -> StatusResult;
        /// Renders (as needed) and returns every tile overlapping the canvas-space rectangle.
        fn text_canvas_resident(renderer: Pin<&mut TextRendererBox>, id: u64, x: i32, y: i32, width: u32, height: u32) -> CanvasTilesResult;
        /// A linear-filtering, clamping sampler to use with the canvas tiles' views (the engine's
        /// tiles carry none). Owned by the renderer; created on first use.
        fn text_canvas_sampler(renderer: Pin<&mut TextRendererBox>) -> u64;
        fn text_canvas_destroy(renderer: Pin<&mut TextRendererBox>, id: u64) -> bool;
    }
}
