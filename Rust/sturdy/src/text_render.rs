//! GPU text rendering: turn shaped text into pixels — into an **offscreen texture** you can sample
//! in your own shaders, onto the **screen** as a UI image, or **into the world** on a quad.
//!
//! A [`TextRenderer`] owns the engine's glyph atlas and text pipeline. [`TextRenderer::create_target`]
//! makes a [`TextTarget`]: an ordinary RHI texture + view + sampler (a color attachment you can also
//! sample), re-renderable any number of times for dynamic text. From a target you can:
//!
//! * bind its RHI handles ([`TextTarget::texture`]/[`view`](TextTarget::view)/[`sampler`](TextTarget::sampler))
//!   in your own [`crate::rhi`] bind groups and shaders — "text as a texture";
//! * [`TextRenderer::adopt`] it as a Renderer texture and draw it with [`crate::ui::Ui::image`]
//!   (screen space, GPU-direct);
//! * [`TextRenderer::read_pixels`] it back and make a texture asset ([`world_label`]) for text on a
//!   quad in the scene.
//!
//! Text is drawn with the engine's distance-field pipeline, so it stays crisp when scaled. Everything
//! is in pixels from the target's top-left, y down. Rendering blocks until the GPU finishes.
//!
//! Create the renderer from `frame` (or later) — it needs the GPU device — and drop it (or call
//! [`TextRenderer::destroy`]) **before** the engine shuts down.

use cxx::UniquePtr;
use glam::{Vec2, Vec4};
use sturdy_sys::text_render::ffi;

use crate::assets::RendererTextureHandle;
use crate::rhi::{Format, SamplerHandle, TextureHandle, TextureViewHandle};
use crate::text::{self, FontId, FontStack, Layout, LayoutOptions, TextError};
use crate::Engine;

/// Glyph atlas sizing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AtlasConfig {
    pub initial_image_size: u32,
    pub maximum_image_size: u32,
    /// Distance-field range in pixels.
    pub pixel_range: f32,
    pub padding_px: f32,
}

impl Default for AtlasConfig {
    fn default() -> Self {
        Self { initial_image_size: 64, maximum_image_size: 4096, pixel_range: 4.0, padding_px: 4.0 }
    }
}

/// One glyph ready to draw: the low-level input to [`TextRenderer::render`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PlacedGlyph {
    /// Baseline pen position in pixels from the target's top-left.
    pub position: Vec2,
    pub rotation: f32,
    /// Straight-alpha RGBA. Color (emoji) glyphs ignore it.
    pub color: Vec4,
    pub font: FontId,
    pub glyph_id: u32,
    pub units_per_em: u32,
    /// The em size in pixels.
    pub pixel_size: f32,
    pub is_color: bool,
    /// Thicken small text slightly so it stays legible.
    pub stem_darkening: bool,
}

impl PlacedGlyph {
    fn to_ffi(self) -> ffi::GlyphPlacementDesc {
        ffi::GlyphPlacementDesc {
            x: self.position.x,
            y: self.position.y,
            rotation: self.rotation,
            color: self.color.into(),
            font_id: self.font.raw(),
            glyph_id: self.glyph_id,
            units_per_em: self.units_per_em,
            pixel_size: self.pixel_size,
            is_color: self.is_color,
            stem_darkening: self.stem_darkening,
        }
    }
}

/// Recolors the glyphs whose source characters fall in a byte range of the text.
#[derive(Debug, Clone, PartialEq)]
pub struct ColorSpan {
    pub bytes: core::ops::Range<usize>,
    pub color: Vec4,
}

/// How to draw a piece of text.
#[derive(Debug, Clone, PartialEq)]
pub struct TextFormat {
    pub fonts: FontStack,
    /// Em size in pixels.
    pub pixel_size: f32,
    pub color: Vec4,
    /// Line wrapping, alignment and OpenType features.
    pub layout: LayoutOptions,
    /// Per-range color overrides.
    pub spans: Vec<ColorSpan>,
    pub stem_darkening: bool,
}

impl TextFormat {
    pub fn new(fonts: FontStack, pixel_size: f32) -> Self {
        Self { fonts, pixel_size, color: Vec4::ONE, layout: LayoutOptions::default(), spans: Vec::new(), stem_darkening: true }
    }

    #[must_use]
    pub fn color(mut self, color: Vec4) -> Self {
        self.color = color;
        self
    }

    /// Wraps at `width_px`.
    #[must_use]
    pub fn wrap(mut self, width_px: f32) -> Self {
        self.layout = self.layout.wrap_at_px(width_px, self.pixel_size);
        self
    }

    #[must_use]
    pub fn align(mut self, alignment: text::Alignment) -> Self {
        self.layout = self.layout.alignment(alignment);
        self
    }

    #[must_use]
    pub fn features(mut self, specification: impl Into<String>) -> Self {
        self.layout = self.layout.features(specification);
        self
    }

    #[must_use]
    pub fn span(mut self, bytes: core::ops::Range<usize>, color: Vec4) -> Self {
        self.spans.push(ColorSpan { bytes, color });
        self
    }

    /// Lays `text` out (no GPU needed) — for measuring.
    pub fn layout(&self, text_str: &str) -> Result<Layout, TextError> {
        text::layout(&self.fonts, text_str, &self.layout)
    }

    /// Size in pixels of `text` drawn with this format.
    pub fn measure(&self, text_str: &str) -> Result<Vec2, TextError> {
        Ok(self.layout(text_str)?.size_px(self.pixel_size))
    }
}

/// A run of text at a position; the unit [`TextRenderer::draw_text`] takes.
#[derive(Debug, Clone, PartialEq)]
pub struct TextItem {
    pub text: String,
    /// Top-left of the text's layout box, in pixels.
    pub origin: Vec2,
    pub format: TextFormat,
}

impl TextItem {
    pub fn new(text: impl Into<String>, origin: Vec2, format: TextFormat) -> Self {
        Self { text: text.into(), origin, format }
    }
}

/// Positions every glyph of a laid-out `text` at `origin` (its layout box's top-left) as drawn at
/// `format.pixel_size`, applying the format's color and spans.
pub fn place_glyphs(layout: &Layout, text_str: &str, origin: Vec2, format: &TextFormat) -> Vec<PlacedGlyph> {
    let px = format.pixel_size;
    layout
        .glyph_positions_with_units()
        .into_iter()
        .map(|(glyph, em_position, units_per_em)| {
            let color = format
                .spans
                .iter()
                .rev()
                .find(|s| s.bytes.contains(&(glyph.cluster as usize)))
                .map_or(format.color, |s| s.color);
            let _ = text_str;
            PlacedGlyph {
                position: origin + em_position * px,
                rotation: 0.0,
                color,
                font: glyph.font,
                glyph_id: glyph.glyph_id,
                units_per_em,
                pixel_size: px,
                is_color: glyph.is_color,
                stem_darkening: format.stem_darkening,
            }
        })
        .collect()
}

/// An offscreen texture text is rendered into. Create with [`TextRenderer::create_target`]; free
/// with [`TextRenderer::destroy_target`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TextTarget {
    id: u64,
    pub width: u32,
    pub height: u32,
    pub format: Format,
    texture: TextureHandle,
    view: TextureViewHandle,
    sampler: SamplerHandle,
}

impl TextTarget {
    /// The RHI texture (color attachment + sampled). Owned by the renderer — do not destroy it
    /// through [`crate::rhi::Rhi`]. After rendering it is in the `ShaderReadOnly` layout.
    pub fn texture(&self) -> TextureHandle {
        self.texture
    }

    pub fn view(&self) -> TextureViewHandle {
        self.view
    }

    /// A linear-filtering sampler for the texture.
    pub fn sampler(&self) -> SamplerHandle {
        self.sampler
    }
}

/// A target's pixels on the CPU.
#[derive(Debug, Clone, PartialEq)]
pub struct Pixels {
    pub width: u32,
    pub height: u32,
    pub bytes_per_pixel: u32,
    /// Tight rows, top to bottom, in the target's format.
    pub data: Vec<u8>,
}

/// Renders shaped text into offscreen targets. See the module docs.
pub struct TextRenderer {
    inner: UniquePtr<ffi::TextRendererBox>,
}

// SAFETY: the C++ object holds GPU handles and plain data with no thread affinity; every use goes
// through `&mut`/`&` and blocks until the GPU work finishes, so moving it between threads is sound.
unsafe impl Send for TextRenderer {}

impl TextRenderer {
    /// Creates a renderer with the default atlas configuration.
    pub fn new(engine: &mut Engine<'_>) -> Result<Self, TextError> {
        Self::with_config(engine, AtlasConfig::default())
    }

    pub fn with_config(engine: &mut Engine<'_>, config: AtlasConfig) -> Result<Self, TextError> {
        let inner = ffi::text_renderer_new(
            engine.view_mut(),
            &ffi::AtlasConfigDesc {
                initial_image_size: config.initial_image_size,
                maximum_image_size: config.maximum_image_size,
                pixel_range: config.pixel_range,
                padding_px: config.padding_px,
            },
        );
        if inner.is_null() {
            return Err(TextError("the engine has no GPU device yet (create the text renderer from `frame`)".into()));
        }
        Ok(Self { inner })
    }

    /// Frees every target and GPU resource now (also done on drop). The renderer is unusable
    /// afterwards.
    pub fn destroy(&mut self) {
        ffi::text_renderer_destroy(self.inner.pin_mut());
    }

    /// Creates a `width`×`height` target in `format` (any color-attachment format; `RGBA8Unorm`
    /// is the usual choice, `RGBA16Float` for HDR).
    pub fn create_target(&mut self, width: u32, height: u32, format: Format) -> Result<TextTarget, TextError> {
        let r = ffi::text_target_create(self.inner.pin_mut(), width, height, format.repr);
        if !r.info.ok {
            return Err(TextError(r.error));
        }
        Ok(Self::target_from(&r.info))
    }

    fn target_from(info: &ffi::TargetInfo) -> TextTarget {
        TextTarget {
            id: info.id,
            width: info.width,
            height: info.height,
            format: Format { repr: info.format },
            texture: TextureHandle { value: info.texture },
            view: TextureViewHandle { value: info.view },
            sampler: SamplerHandle { value: info.sampler },
        }
    }

    /// Clears `target` to transparent and draws pre-placed `glyphs`.
    pub fn render(&mut self, target: &TextTarget, glyphs: &[PlacedGlyph]) -> Result<(), TextError> {
        let desc: Vec<ffi::GlyphPlacementDesc> = glyphs.iter().map(|g| g.to_ffi()).collect();
        let r = ffi::text_target_render(self.inner.pin_mut(), target.id, &desc);
        if r.ok { Ok(()) } else { Err(TextError(r.error)) }
    }

    /// Clears `target` and draws every item (each laid out with its own format).
    pub fn draw_text(&mut self, target: &TextTarget, items: &[TextItem]) -> Result<(), TextError> {
        let mut glyphs = Vec::new();
        for item in items {
            let layout = item.format.layout(&item.text)?;
            glyphs.extend(place_glyphs(&layout, &item.text, item.origin, &item.format));
        }
        self.render(target, &glyphs)
    }

    /// Copies the target's pixels back to the CPU.
    pub fn read_pixels(&mut self, target: &TextTarget) -> Result<Pixels, TextError> {
        let r = ffi::text_target_read(self.inner.pin_mut(), target.id);
        check(r.ok, r.error)?;
        Ok(Pixels { width: r.width, height: r.height, bytes_per_pixel: r.bytes_per_pixel, data: r.pixels })
    }

    /// Registers the target as a Renderer texture so [`crate::ui::Ui::image`] can draw it. The
    /// handle is valid until the target is destroyed.
    pub fn adopt(&self, engine: &mut Engine<'_>, target: &TextTarget) -> Option<RendererTextureHandle> {
        let raw = ffi::text_target_adopt(&self.inner, engine.view_mut(), target.id);
        (raw != 0).then_some(RendererTextureHandle(raw))
    }

    /// Frees a target and its GPU resources.
    pub fn destroy_target(&mut self, target: TextTarget) -> bool {
        ffi::text_target_destroy(self.inner.pin_mut(), target.id)
    }
}

fn check(ok: bool, error: String) -> Result<(), TextError> {
    if ok { Ok(()) } else { Err(TextError(error)) }
}

// -- Text canvas -------------------------------------------------------------------------------

/// A huge virtual text surface (`i32` coordinates, y down) backed by lazily rendered square tiles.
/// Draw text anywhere on it, then ask for the tiles overlapping the region you want to show: only
/// those are rendered (and kept, up to a budget), so a canvas can be millions of pixels across. Made
/// with [`TextRenderer::create_canvas`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TextCanvas {
    id: u64,
    /// Side of a tile in pixels (the requested size clamped to the device limit).
    pub tile_size: u32,
}

/// A resident canvas tile: an RGBA8 texture view holding the canvas pixels `logical_position ..
/// logical_position + logical_size`. Composite the tiles covering your viewport (sample each with
/// [`TextRenderer::canvas_sampler`]); transparent where nothing was drawn. A tile is valid until the
/// next [`TextRenderer::canvas_tiles`] call that evicts it, or until the canvas is destroyed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CanvasTile {
    /// Tile grid coordinate (`logical_position / tile_size`).
    pub coord: glam::IVec2,
    pub view: TextureViewHandle,
    pub logical_position: glam::IVec2,
    pub logical_size: glam::UVec2,
}

impl TextRenderer {
    /// Creates a canvas with `desired_tile_size`-pixel tiles, keeping at most `max_resident_tiles` in
    /// GPU memory (least recently requested are evicted).
    pub fn create_canvas(&mut self, desired_tile_size: u32, max_resident_tiles: u32) -> Result<TextCanvas, TextError> {
        let r = ffi::text_canvas_create(self.inner.pin_mut(), desired_tile_size, max_resident_tiles);
        check(r.ok, r.error)?;
        Ok(TextCanvas { id: r.id, tile_size: r.tile_size })
    }

    /// Adds pre-placed glyphs at canvas coordinates. Already-resident tiles they touch are
    /// re-rendered on next use.
    pub fn canvas_draw(&mut self, canvas: &TextCanvas, glyphs: &[PlacedGlyph]) -> Result<(), TextError> {
        let desc: Vec<ffi::GlyphPlacementDesc> = glyphs.iter().map(|g| g.to_ffi()).collect();
        let r = ffi::text_canvas_draw(self.inner.pin_mut(), canvas.id, &desc);
        check(r.ok, r.error)
    }

    /// Lays out and adds every item (positions are canvas coordinates). Unlike
    /// [`TextRenderer::draw_text`] this **adds** to the canvas rather than clearing it.
    pub fn canvas_draw_text(&mut self, canvas: &TextCanvas, items: &[TextItem]) -> Result<(), TextError> {
        let mut glyphs = Vec::new();
        for item in items {
            let layout = item.format.layout(&item.text)?;
            glyphs.extend(place_glyphs(&layout, &item.text, item.origin, &item.format));
        }
        self.canvas_draw(canvas, &glyphs)
    }

    /// Renders any missing/dirty tiles overlapping the canvas rectangle and returns them (blocks
    /// until the GPU is done).
    pub fn canvas_tiles(&mut self, canvas: &TextCanvas, position: glam::IVec2, size: glam::UVec2) -> Result<Vec<CanvasTile>, TextError> {
        let r = ffi::text_canvas_resident(self.inner.pin_mut(), canvas.id, position.x, position.y, size.x, size.y);
        check(r.ok, r.error)?;
        Ok(r
            .tiles
            .into_iter()
            .map(|t| CanvasTile {
                coord: glam::IVec2::new(t.tile_x, t.tile_y),
                view: TextureViewHandle { value: t.view },
                logical_position: glam::IVec2::new(t.logical_x, t.logical_y),
                logical_size: glam::UVec2::new(t.logical_width, t.logical_height),
            })
            .collect())
    }

    /// A linear-filtering, clamping sampler for canvas tile views (the engine's tiles have none).
    pub fn canvas_sampler(&mut self) -> Option<SamplerHandle> {
        let raw = ffi::text_canvas_sampler(self.inner.pin_mut());
        (raw != 0).then_some(SamplerHandle { value: raw })
    }

    /// Frees the canvas and its tiles.
    pub fn destroy_canvas(&mut self, canvas: TextCanvas) -> bool {
        ffi::text_canvas_destroy(self.inner.pin_mut(), canvas.id)
    }
}

// -- Text in the world -------------------------------------------------------------------------

/// Text baked onto a quad model, ready to place in the scene. See [`world_label`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WorldLabel {
    /// The quad model. Spawn it with `scene::ModelRenderer::new(label.model)` + a `WorldTransform`
    /// (the quad faces +Z, is `size_meters` big and centered on its origin).
    pub model: crate::assets::Asset,
    /// The text texture asset the model samples.
    pub texture: crate::assets::Asset,
    pub size_meters: Vec2,
}

/// Renders `items` into a temporary target sized `width_px`×`height_px`, turns the pixels into a
/// texture asset and puts it on a double-sided, self-lit quad `height_meters` tall (width follows the
/// pixel aspect). Text edges are alpha-cut at 0.5 by the deferred material (it has no true
/// transparency), so use a large enough `pixel_size` for smooth edges; the mesh is a plain quad you
/// can billboard by rotating its transform yourself.
///
/// Because it goes through the CPU it suits labels that change rarely; for per-frame text use a
/// [`TextTarget`] directly. `shader_path` is the deferred geometry shader (the engine's own is
/// `"Shaders/gbuffer_geometry.slang"`).
#[allow(clippy::too_many_arguments)]
pub fn world_label(
    engine: &mut Engine<'_>,
    renderer: &mut TextRenderer,
    items: &[TextItem],
    width_px: u32,
    height_px: u32,
    height_meters: f32,
    shader_path: &str,
    label: &str,
) -> Result<WorldLabel, TextError> {
    use crate::assets::{
        ModelBuilder, ModelPrimitive, ModelVertex, TextureColorSpace, TextureKind, TexturePixelFormat,
    };

    let target = renderer.create_target(width_px, height_px, Format::RGBA8Unorm)?;
    let drawn = renderer.draw_text(&target, items).and_then(|()| renderer.read_pixels(&target));
    renderer.destroy_target(target);
    let pixels = drawn?;

    let mut assets = engine.assets();
    let asset_err = |e: crate::assets::AssetError| TextError(format!("{e}"));
    let texture = assets
        .create_texture(
            &pixels.data,
            pixels.width,
            pixels.height,
            TexturePixelFormat::Rgba8,
            TextureColorSpace::Srgb,
            TextureKind::ColorAlpha,
            format!("{label} (text)"),
            false,
            true,
        )
        .map_err(asset_err)?;
    let shader = assets.load_shader(shader_path, format!("{label} (shader)")).map_err(asset_err)?;

    let size = Vec2::new(height_meters * width_px as f32 / height_px as f32, height_meters);
    let (hw, hh) = (size.x * 0.5, size.y * 0.5);
    let vertex = |x: f32, y: f32, u: f32, v: f32| ModelVertex {
        position: glam::Vec3::new(x, y, 0.0),
        normal: glam::Vec3::Z,
        uv: Vec2::new(u, v),
        color: Vec4::ONE,
        tangent: Vec4::new(1.0, 0.0, 0.0, 1.0),
    };
    let primitive = ModelPrimitive::new(
        vec![vertex(-hw, hh, 0.0, 0.0), vertex(hw, hh, 1.0, 0.0), vertex(hw, -hh, 1.0, 1.0), vertex(-hw, -hh, 0.0, 1.0)],
        vec![0, 3, 2, 0, 2, 1],
        shader,
    )
    .with_texture("base_color_texture", texture)
    .with_texture("emissive_texture", texture)
    .with_double_sided(true);
    let model = assets.create_model(ModelBuilder::new(label).with_primitive(primitive)).map_err(asset_err)?;
    assets.set_model_float(model, 0, "alpha_cutoff", 0.5).map_err(asset_err)?;
    assets.set_model_float(model, 0, "emissive_strength", 1.0).map_err(asset_err)?;
    assets.set_model_vec4(model, 0, "emissive_factor", Vec4::ONE).map_err(asset_err)?;
    Ok(WorldLabel { model, texture, size_meters: size })
}

// -- Text on the screen ------------------------------------------------------------------------

impl crate::ui::Ui<'_> {
    /// Uses a font loaded with [`crate::text`] as UI font `ui_font_id`, so UI text and widgets can
    /// draw with it (`TextStyle::font_id`). Not copied: keep `font` loaded while the UI runs.
    pub fn register_text_font(&mut self, font: FontId, ui_font_id: u16) -> bool {
        sturdy_sys::ui::ffi::ui_register_text_font(font.raw(), ui_font_id)
    }

    /// Free-draws `text` on the screen with its top-left at `position` (pixels from the window's
    /// top-left), independent of the surrounding layout: a floating root-attached element that
    /// blocks no input. Uses the UI's distance-field text (crisp at any scale) and
    /// [`crate::ui::TextStyle`] (font, size, color, wrapping, alignment).
    pub fn draw_text(&mut self, position: Vec2, text_str: &str, style: &crate::ui::TextStyle) {
        use crate::ui::{ElementDesc, FloatTarget, Floating, Sizing};
        let floating = Floating {
            attach_to: FloatTarget::Root,
            element_attach_point: crate::ui::AttachPoint::LeftTop,
            parent_attach_point: crate::ui::AttachPoint::LeftTop,
            offset: position,
            z_index: i16::MAX,
            capture_pointer: false,
            clip_to_target: false,
        };
        let desc = ElementDesc::new().width(Sizing::Fit).height(Sizing::Fit).floating(Some(floating));
        let scope = self.element(&desc);
        self.text(text_str, style);
        drop(scope);
    }

    /// Draws a [`TextTarget`] (adopted with [`TextRenderer::adopt`]) on the screen at `position`,
    /// `size` pixels big.
    pub fn draw_text_target(&mut self, position: Vec2, size: Vec2, texture: RendererTextureHandle) {
        use crate::ui::{ElementDesc, FloatTarget, Floating, Sizing};
        let floating = Floating {
            attach_to: FloatTarget::Root,
            element_attach_point: crate::ui::AttachPoint::LeftTop,
            parent_attach_point: crate::ui::AttachPoint::LeftTop,
            offset: position,
            z_index: i16::MAX,
            capture_pointer: false,
            clip_to_target: false,
        };
        let desc = ElementDesc::new().width(Sizing::Fixed(size.x)).height(Sizing::Fixed(size.y)).floating(Some(floating));
        self.image(&desc, texture);
    }
}
