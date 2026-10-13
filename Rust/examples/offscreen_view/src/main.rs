//! An embedded 3D viewer: the window is UI-only, and the cube in the middle of it is a separate camera's view of the
//! scene rendered into an offscreen target. The cube lives on layer bit 31 and only the viewer camera sees that layer.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Instant;

use sturdy::assets::{ModelBuilder, ModelPrimitive, RendererTextureHandle, Shape};
use sturdy::camera::Camera;
use sturdy::glam::{vec3, vec4, UVec2};
use sturdy::prelude::*;
use sturdy::render::{OffscreenTargetDesc, OffscreenTargetHandle};
use sturdy::scene::{ModelRenderer, WorldTransform};
use sturdy::ui::{self, Direction, ElementDesc, Sizing, TextStyle};

const VIEWER_LAYER: u32 = 1 << 31;

struct Viewer {
    start: Instant,
    target: Option<OffscreenTargetHandle>,
    texture: Arc<AtomicU64>,
    renders: u32,
    error: Option<String>,
}

impl GameLogic for Viewer {
    fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
        let target = engine.create_offscreen_target(&OffscreenTargetDesc::new(UVec2::new(320, 320)).label("viewer"))?;
        self.texture.store(engine.offscreen_target_texture(target), Ordering::Relaxed);
        self.target = Some(target);

        let shader = engine.assets().load_shader("Shaders/gbuffer_geometry.slang", "viewer shader").map_err(|e| Error::new(format!("{e:?}")))?;
        let cube = ModelPrimitive::shape(Shape::CUBE, shader)
            .ok_or_else(|| Error::new("cube"))?
            .with_vertex_color(vec4(0.9, 0.3, 0.2, 1.0));
        let model = engine.assets().create_model(ModelBuilder::new("viewer cube").with_primitive(cube)).map_err(|e| Error::new(format!("{e:?}")))?;
        let _ = engine
            .ecs()
            .spawn_builder()
            .with(WorldTransform::from_translation([0.0, 0.0, 0.0]))
            .with(ModelRenderer::new(model).with_visibility_mask(VIEWER_LAYER))
            .build()
            .map_err(|e| Error::new(format!("{e:?}")))?;

        let texture = self.texture.clone();
        ui::install_draw_hook(move |ui| {
            let _root = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Grow).direction(Direction::TopToBottom));
            ui.text("embedded 3D view:", &TextStyle::default());
            ui.image(
                &ElementDesc::new().width(Sizing::Fixed(320.0)).height(Sizing::Fixed(320.0)),
                RendererTextureHandle::from_raw(texture.load(Ordering::Relaxed)),
            );
        });
        Ok(())
    }

    fn frame(&mut self, engine: &mut Engine<'_>, _frame: &Frame) -> Option<FrameDescription> {
        if let Some(target) = self.target {
            let camera = Camera::default().at(vec3(0.0, 0.0, 3.5)).culling_mask(VIEWER_LAYER);
            let view = FrameDescription::new(camera);
            match engine.render_offscreen_view(target, &view) {
                Ok(()) => self.renders += 1,
                Err(e) => self.error = Some(e.to_string()),
            }
        }
        if self.start.elapsed().as_secs() >= 8 {
            println!("[offscreen_view] {} offscreen renders, error: {:?}", self.renders, self.error);
            if let Some(mut window) = engine.primary_window_control() {
                window.request_close();
            }
        }
        Some(FrameDescription::ui_only())
    }
}

fn main() -> std::process::ExitCode {
    sturdy::run(
        RuntimeConfig::new("offscreen_view").size(480, 420),
        Viewer { start: Instant::now(), target: None, texture: Arc::new(AtomicU64::new(0)), renders: 0, error: None },
    )
}
