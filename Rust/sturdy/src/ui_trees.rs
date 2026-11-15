//! UI trees: layouts the engine builds once per update and shows over a window, in an off-screen render target (a panel in the world, UI as
//! a texture) or nowhere at all (headless, for tools and tests).
//!
//! [`ui::install_draw_hook`](crate::ui::install_draw_hook) draws one UI over the primary window each frame. A tree is the general form:
//! any number of them, each with its own target, stacking layer, scale, rebuild policy and pointer input. Its builder is a closure that
//! gets a [`Ui`](crate::ui::Ui) bound to that tree and declares the elements, exactly as a draw hook does:
//!
//! ```no_run
//! use sturdy::ui::{Direction, ElementDesc, Sizing};
//! use sturdy::ui_trees::{RebuildPolicy, TreeTarget, UiTreeDesc, UiTrees};
//!
//! fn panel(engine: &mut sturdy::Engine<'_>, target: sturdy::render::OffscreenTargetHandle) -> Result<(), String> {
//!     let desc = UiTreeDesc::new("terminal")
//!         .target(TreeTarget::Offscreen(target))
//!         .rebuild(RebuildPolicy::OnDirty);
//!     let tree = UiTrees::new(engine).spawn(desc, |ui, _frame| {
//!         let _column = ui.element(&ElementDesc::new().width(Sizing::Grow).direction(Direction::TopToBottom));
//!     })?;
//!     // Each frame: tell the tree where the pointer is on the panel (0..1 across it) and draw the targets that changed.
//!     tree.point_at(engine, Some(sturdy::glam::vec2(0.5, 0.5)), false)?;
//!     UiTrees::new(engine).render_targets()?;
//!     Ok(())
//! }
//! ```
//!
//! Trees change the ECS world, so spawn and destroy them from game logic (`GameLogic::frame`, a system), never from inside a UI hook or
//! another tree's builder.

use std::pin::Pin;

use sturdy_sys::ui::{ffi, EngineView, UiTreeBuilderHandle, UiTreeDirtyHandle};

use crate::ecs::Entity;
use crate::render::OffscreenTargetHandle;
use crate::ui::{HitTestMode, Ui};
use crate::Engine;

/// Where a tree's layout is shown.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TreeTarget {
    /// Drawn over a window's frames; `None` is the primary window.
    Window(Option<u64>),
    /// Drawn into an off-screen render target (see `Engine::create_offscreen_target`) at the target's size. Sample the target on a mesh to
    /// show the panel in the world; feed its pointer with [`UiTree::point_at`].
    Offscreen(OffscreenTargetHandle),
    /// Laid out at this size in pixels and drawn nowhere.
    Headless { width: u32, height: u32 },
}

impl Default for TreeTarget {
    fn default() -> Self {
        Self::Window(None)
    }
}

impl TreeTarget {
    fn parts(self) -> (u32, u64, u64, u32, u32) {
        match self {
            Self::Window(window) => (0, window.unwrap_or(0), 0, 0, 0),
            Self::Offscreen(target) => (1, 0, target.value, 0, 0),
            Self::Headless { width, height } => (2, 0, 0, width, height),
        }
    }
}

/// When a tree is laid out again. Whatever the policy, a tree is rebuilt when its extent changes or after [`UiTree::request_rebuild`].
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub enum RebuildPolicy {
    /// Every update. Right for a HUD that shows live values.
    #[default]
    EveryUpdate,
    /// Only when input reached the tree, a rebuild was requested (by the app, or by the builder for an animation in progress), or the
    /// extent changed. Right for in-world panels and mostly static tools.
    OnDirty,
    /// At most this many times a second.
    FixedRate(f32),
}

impl RebuildPolicy {
    fn parts(self) -> (u32, f32) {
        match self {
            Self::EveryUpdate => (0, 30.0),
            Self::OnDirty => (1, 30.0),
            Self::FixedRate(hz) => (2, hz),
        }
    }
}

/// Whether a tree's layout units follow the content scale of what it is drawn on or are raw pixels.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TreeScaleMode {
    #[default]
    ContentScale,
    RawPixels,
}

/// What a tree is made of, besides its builder.
#[derive(Debug, Clone, PartialEq)]
pub struct UiTreeDesc {
    /// For logs, stats and the overlay pass name.
    pub name: String,
    pub target: TreeTarget,
    pub rebuild: RebuildPolicy,
    /// Stacking among trees on the same window: higher draws on top and gets the pointer first.
    pub layer: i32,
    pub scale_mode: TreeScaleMode,
    /// Content scale for off-screen and headless trees (window trees follow their window's). 1 = a layout unit is a pixel.
    pub content_scale: f32,
    /// HDR displays only: scales the reference white the UI is composed at.
    pub hdr_reference_white_scale: f32,
    /// Window trees only: whether the engine feeds the tree from its window's mouse, keyboard and text events.
    pub window_input: bool,
    /// Off-screen trees only: what the target is cleared to before the UI is drawn (straight-alpha linear RGBA).
    pub clear_color: glam::Vec4,
    /// A build longer than this is counted in the stats and logged.
    pub build_budget_ms: f32,
    pub hit_test_mode: HitTestMode,
    /// Lay this tree out on the engine's worker threads together with the other parallel trees instead of one at a time on the update
    /// thread. The builder is `Send` either way; this also asks for it to run beside other builders, so it must not rely on anything that
    /// is only valid on the update thread.
    pub parallel_build: bool,
}

impl UiTreeDesc {
    pub fn new(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            target: TreeTarget::default(),
            rebuild: RebuildPolicy::default(),
            layer: 0,
            scale_mode: TreeScaleMode::default(),
            content_scale: 1.0,
            hdr_reference_white_scale: 1.0,
            window_input: true,
            clear_color: glam::Vec4::new(0.0, 0.0, 0.0, 1.0),
            build_budget_ms: 4.0,
            hit_test_mode: HitTestMode::default(),
            parallel_build: false,
        }
    }

    #[must_use]
    pub fn target(mut self, target: TreeTarget) -> Self {
        self.target = target;
        self
    }

    #[must_use]
    pub fn rebuild(mut self, rebuild: RebuildPolicy) -> Self {
        self.rebuild = rebuild;
        self
    }

    #[must_use]
    pub fn layer(mut self, layer: i32) -> Self {
        self.layer = layer;
        self
    }

    #[must_use]
    pub fn scale_mode(mut self, scale_mode: TreeScaleMode) -> Self {
        self.scale_mode = scale_mode;
        self
    }

    #[must_use]
    pub fn content_scale(mut self, content_scale: f32) -> Self {
        self.content_scale = content_scale;
        self
    }

    #[must_use]
    pub fn hdr_reference_white_scale(mut self, scale: f32) -> Self {
        self.hdr_reference_white_scale = scale;
        self
    }

    #[must_use]
    pub fn window_input(mut self, window_input: bool) -> Self {
        self.window_input = window_input;
        self
    }

    #[must_use]
    pub fn clear_color(mut self, clear_color: glam::Vec4) -> Self {
        self.clear_color = clear_color;
        self
    }

    #[must_use]
    pub fn build_budget_ms(mut self, budget: f32) -> Self {
        self.build_budget_ms = budget;
        self
    }

    #[must_use]
    pub fn hit_test_mode(mut self, mode: HitTestMode) -> Self {
        self.hit_test_mode = mode;
        self
    }

    #[must_use]
    pub fn parallel_build(mut self, parallel: bool) -> Self {
        self.parallel_build = parallel;
        self
    }

    fn to_ffi(&self) -> ffi::UiTreeDescInfo {
        let (target_kind, window, offscreen_target, headless_width, headless_height) = self.target.parts();
        let (rebuild, rebuild_hz) = self.rebuild.parts();
        ffi::UiTreeDescInfo {
            name: self.name.clone(),
            target_kind,
            window,
            offscreen_target,
            headless_width,
            headless_height,
            rebuild,
            rebuild_hz,
            layer: self.layer,
            raw_pixels: self.scale_mode == TreeScaleMode::RawPixels,
            content_scale: self.content_scale,
            hdr_reference_white_scale: self.hdr_reference_white_scale,
            window_input: self.window_input,
            clear_color: self.clear_color.into(),
            build_budget_ms: self.build_budget_ms,
            hit_test_fixed: self.hit_test_mode == HitTestMode::Fixed,
            parallel_build: self.parallel_build,
        }
    }
}

/// What a builder is told about the layout it is making.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TreeFrame {
    /// The tree's own entity.
    pub entity: Entity,
    /// The size being laid out, in layout units.
    pub extent: glam::Vec2,
    pub delta_seconds: f32,
    /// How many times this tree has been laid out before.
    pub build_index: u64,
    /// Set this to lay the tree out again next update even under an on-dirty or fixed-rate policy (an animation still running).
    pub request_rebuild: bool,
}

/// What the engine measured about a tree.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct TreeStats {
    pub builds: u64,
    /// Updates in which the tree was not rebuilt because its policy said it was clean.
    pub skipped: u64,
    pub over_budget: u64,
    pub failed_builds: u64,
    /// Off-screen trees: frames rendered into the target.
    pub target_renders: u64,
    pub last_build_ms: f64,
    pub max_build_ms: f64,
    /// Extent of the latest finished layout (0 before the first).
    pub last_extent: glam::Vec2,
    /// Message of the last failed build; empty if none.
    pub last_error: String,
}

/// A spawned tree. A cheap handle: its calls take the engine they act on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct UiTree {
    entity: Entity,
}

fn checked(outcome: ffi::UiTreeOutcome) -> Result<ffi::UiTreeOutcome, String> {
    if outcome.ok {
        Ok(outcome)
    } else {
        Err(outcome.message)
    }
}

/// Wraps a builder closure for the engine: it gets a [`Ui`] bound to the tree being laid out.
fn builder_handle<F>(mut build: F) -> Box<UiTreeBuilderHandle>
where
    F: FnMut(&mut Ui<'_>, &mut TreeFrame) + Send + 'static,
{
    Box::new(UiTreeBuilderHandle {
        build: Box::new(move |view: Pin<&mut EngineView>, info: &ffi::UiTreeFrameInfo| {
            let mut frame = TreeFrame {
                entity: Entity { index: info.entity_index, generation: info.entity_generation },
                extent: glam::Vec2::new(info.width, info.height),
                delta_seconds: info.delta_seconds,
                build_index: info.build_index,
                request_rebuild: false,
            };
            let mut ui = Ui::new(view);
            build(&mut ui, &mut frame);
            frame.request_rebuild
        }),
    })
}

impl UiTree {
    /// The tree's entity.
    pub fn entity(self) -> Entity {
        self.entity
    }

    /// Replaces the builder (the layout is rebuilt next update).
    pub fn set_builder<F>(self, engine: &mut Engine<'_>, build: F) -> Result<(), String>
    where
        F: FnMut(&mut Ui<'_>, &mut TreeFrame) + Send + 'static,
    {
        checked(ffi::ui_tree_set_builder(engine.view_mut(), self.entity.index, self.entity.generation, builder_handle(build))).map(|_| ())
    }

    /// Moves the tree to another target (the next layout is for it).
    pub fn set_target(self, engine: &mut Engine<'_>, target: TreeTarget) -> Result<(), String> {
        let (kind, window, offscreen, width, height) = target.parts();
        checked(ffi::ui_tree_set_target(
            engine.view_mut(),
            self.entity.index,
            self.entity.generation,
            kind,
            window,
            offscreen,
            width,
            height,
        ))
        .map(|_| ())
    }

    pub fn set_rebuild_policy(self, engine: &mut Engine<'_>, policy: RebuildPolicy) -> Result<(), String> {
        let (policy, hz) = policy.parts();
        checked(ffi::ui_tree_set_rebuild_policy(engine.view_mut(), self.entity.index, self.entity.generation, policy, hz)).map(|_| ())
    }

    /// Stacking among trees on the same window: higher draws on top and gets the pointer first.
    pub fn set_layer(self, engine: &mut Engine<'_>, layer: i32) -> Result<(), String> {
        checked(ffi::ui_tree_set_layer(engine.view_mut(), self.entity.index, self.entity.generation, layer)).map(|_| ())
    }

    pub fn layer(self, engine: &mut Engine<'_>) -> Result<i32, String> {
        checked(ffi::ui_tree_layer(engine.view_mut(), self.entity.index, self.entity.generation)).map(|o| o.value as i64 as i32)
    }

    /// Lays the tree out again next update, whatever its policy.
    pub fn request_rebuild(self, engine: &mut Engine<'_>) -> Result<(), String> {
        checked(ffi::ui_tree_request_rebuild(engine.view_mut(), self.entity.index, self.entity.generation)).map(|_| ())
    }

    /// Feeds an off-screen tree's pointer: `uv` is where it is on the panel, 0..1 across and down (a ray against the mesh, a gamepad cursor);
    /// `None` when it left. `down` is the primary button.
    pub fn point_at(self, engine: &mut Engine<'_>, uv: Option<glam::Vec2>, down: bool) -> Result<(), String> {
        let (has_uv, u, v) = uv.map_or((false, 0.0, 0.0), |uv| (true, uv.x, uv.y));
        checked(ffi::ui_tree_point_at(engine.view_mut(), self.entity.index, self.entity.generation, has_uv, u, v, down)).map(|_| ())
    }

    /// Feeds a scroll wheel movement to the tree.
    pub fn scroll(self, engine: &mut Engine<'_>, dx: f32, dy: f32) -> Result<(), String> {
        checked(ffi::ui_tree_scroll(engine.view_mut(), self.entity.index, self.entity.generation, dx, dy)).map(|_| ())
    }

    /// Feeds typed text to the tree.
    pub fn add_text(self, engine: &mut Engine<'_>, text: &str) -> Result<(), String> {
        checked(ffi::ui_tree_add_text(engine.view_mut(), self.entity.index, self.entity.generation, text)).map(|_| ())
    }

    /// Whether the tree's elements took the pointer in the latest frame, so the game should not also act on it.
    pub fn pointer_consumed(self, engine: &mut Engine<'_>) -> Result<bool, String> {
        checked(ffi::ui_tree_pointer_consumed(engine.view_mut(), self.entity.index, self.entity.generation)).map(|o| o.flag)
    }

    pub fn stats(self, engine: &mut Engine<'_>) -> Result<TreeStats, String> {
        let stats = ffi::ui_tree_stats(engine.view_mut(), self.entity.index, self.entity.generation);
        if !stats.ok {
            return Err(stats.message);
        }
        Ok(TreeStats {
            builds: stats.builds,
            skipped: stats.skipped,
            over_budget: stats.over_budget,
            failed_builds: stats.failed_builds,
            target_renders: stats.target_renders,
            last_build_ms: stats.last_build_ms,
            max_build_ms: stats.max_build_ms,
            last_extent: glam::Vec2::new(stats.last_width, stats.last_height),
            last_error: stats.last_error,
        })
    }

    /// Removes the tree; its builder is dropped.
    pub fn destroy(self, engine: &mut Engine<'_>) -> Result<(), String> {
        checked(ffi::ui_tree_destroy(engine.view_mut(), self.entity.index, self.entity.generation)).map(|_| ())
    }
}

/// Tree calls that are not about one tree, borrowed from the engine for the duration of the call site.
pub struct UiTrees<'e, 'a> {
    engine: &'e mut Engine<'a>,
}

impl<'e, 'a> UiTrees<'e, 'a> {
    pub fn new(engine: &'e mut Engine<'a>) -> Self {
        Self { engine }
    }

    /// Spawns a tree. `build` runs once per layout (on the update thread, or on a worker for a parallel tree) with a [`Ui`] bound to the
    /// tree; it declares elements the way a draw hook does.
    pub fn spawn<F>(&mut self, desc: UiTreeDesc, build: F) -> Result<UiTree, String>
    where
        F: FnMut(&mut Ui<'_>, &mut TreeFrame) + Send + 'static,
    {
        self.spawn_inner(desc, build, None)
    }

    /// [`UiTrees::spawn`] with a dirty check: asked once per update before the rebuild decision, on the update thread; returning true marks
    /// the tree dirty so an on-dirty or fixed-rate tree is laid out this update. For trees that mirror state kept elsewhere. Keep it cheap.
    pub fn spawn_with_dirty_check<F, D>(&mut self, desc: UiTreeDesc, build: F, dirty: D) -> Result<UiTree, String>
    where
        F: FnMut(&mut Ui<'_>, &mut TreeFrame) + Send + 'static,
        D: FnMut() -> bool + Send + 'static,
    {
        self.spawn_inner(desc, build, Some(Box::new(dirty)))
    }

    fn spawn_inner<F>(&mut self, desc: UiTreeDesc, build: F, dirty: Option<Box<dyn FnMut() -> bool + Send>>) -> Result<UiTree, String>
    where
        F: FnMut(&mut Ui<'_>, &mut TreeFrame) + Send + 'static,
    {
        let has_dirty = dirty.is_some();
        let check = dirty.unwrap_or_else(|| Box::new(|| false));
        let outcome = checked(ffi::ui_tree_spawn(
            self.engine.view_mut(),
            &desc.to_ffi(),
            builder_handle(build),
            Box::new(UiTreeDirtyHandle { check }),
            has_dirty,
        ))?;
        Ok(UiTree { entity: Entity { index: outcome.index, generation: outcome.generation } })
    }

    /// How many trees the engine laid out at its last build.
    pub fn count(&mut self) -> Result<u64, String> {
        checked(ffi::ui_tree_count(self.engine.view_mut())).map(|o| o.value)
    }

    /// Renders every off-screen tree that has a frame its target has not shown yet; returns how many it rendered. The application loop does
    /// this after each update; call it yourself only when you drive frames yourself.
    pub fn render_targets(&mut self) -> Result<u64, String> {
        checked(ffi::ui_tree_render_targets(self.engine.view_mut())).map(|o| o.value)
    }
}
