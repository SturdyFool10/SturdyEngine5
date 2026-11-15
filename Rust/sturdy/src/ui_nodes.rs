//! Retained UI as entities: every node is an ECS entity carrying one layout element, nodes link to a parent node, and a node tree lays
//! out the nodes under a root whenever one of them changes.
//!
//! Where a draw hook ([`crate::ui::install_draw_hook`]) re-declares the whole UI each frame, nodes are data you keep: create, edit,
//! reparent and destroy them from game logic and the UI follows. Node calls change the ECS world, so make them from game logic
//! (`GameLogic::frame`, a system), never from inside a UI hook.
//!
//! ```no_run
//! use sturdy::ui::{Direction, ElementDesc, Sizing};
//! use sturdy::ui_nodes::{UiNodeDesc, UiNodes};
//!
//! fn build(engine: &mut sturdy::Engine<'_>) -> Result<(), String> {
//!     let mut nodes = UiNodes::new(engine);
//!     let root = nodes.spawn(&UiNodeDesc::new(ElementDesc::new().width(Sizing::Grow).direction(Direction::TopToBottom)))?;
//!     nodes.spawn(&UiNodeDesc::new(ElementDesc::new().id("title".to_string())).text("Inventory").parent(root))?;
//!     nodes.spawn_tree(root, "inventory", 0)?;
//!     Ok(())
//! }
//! ```

use sturdy_sys::ui::ffi;

use crate::ecs::Entity;
use crate::ui::{ElementDesc, TextStyle};
use crate::Engine;

/// One node's data.
#[derive(Debug, Clone, PartialEq)]
pub struct UiNodeDesc {
    /// The element: sizing, padding, colours, corner radii and shapes, effect, and so on. A node with an `id` reports hover and click.
    pub element: ElementDesc,
    /// Drawn inside the element after its child nodes.
    pub text: String,
    pub text_style: TextStyle,
    /// Siblings are laid out in ascending order, ties in creation order.
    pub order: i32,
    /// A hidden node and everything under it is skipped.
    pub visible: bool,
    /// `None` makes the node a root.
    pub parent: Option<Entity>,
}

impl UiNodeDesc {
    pub fn new(element: ElementDesc) -> Self {
        Self { element, text: String::new(), text_style: TextStyle::default(), order: 0, visible: true, parent: None }
    }

    #[must_use]
    pub fn text(mut self, text: impl Into<String>) -> Self {
        self.text = text.into();
        self
    }

    #[must_use]
    pub fn text_style(mut self, style: TextStyle) -> Self {
        self.text_style = style;
        self
    }

    #[must_use]
    pub fn order(mut self, order: i32) -> Self {
        self.order = order;
        self
    }

    #[must_use]
    pub fn visible(mut self, visible: bool) -> Self {
        self.visible = visible;
        self
    }

    #[must_use]
    pub fn parent(mut self, parent: Entity) -> Self {
        self.parent = Some(parent);
        self
    }

    fn to_ffi(&self) -> ffi::UiNodeDesc {
        let parent = self.parent.unwrap_or(Entity { index: 0, generation: 0 });
        ffi::UiNodeDesc {
            element: self.element.to_ffi(),
            text: self.text.clone(),
            text_style: self.text_style.to_ffi(),
            order: self.order,
            visible: self.visible,
            parent_index: parent.index,
            parent_generation: parent.generation,
        }
    }
}

/// What the last layout found for a node that has an element id.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct UiNodeState {
    pub hovered: bool,
    pub clicked: bool,
}

/// Node calls, borrowed from the engine for the duration of the call site.
pub struct UiNodes<'e, 'a> {
    engine: &'e mut Engine<'a>,
}

fn entity_of(outcome: &ffi::UiNodeOutcome) -> Entity {
    Entity { index: outcome.index, generation: outcome.generation }
}

fn checked(outcome: ffi::UiNodeOutcome) -> Result<ffi::UiNodeOutcome, String> {
    if outcome.ok {
        Ok(outcome)
    } else {
        Err(outcome.message)
    }
}

impl<'e, 'a> UiNodes<'e, 'a> {
    pub fn new(engine: &'e mut Engine<'a>) -> Self {
        Self { engine }
    }

    /// Creates a node entity.
    pub fn spawn(&mut self, node: &UiNodeDesc) -> Result<Entity, String> {
        checked(ffi::ui_node_spawn(self.engine.view_mut(), &node.to_ffi())).map(|o| entity_of(&o))
    }

    /// Replaces a node's data and parent. Refuses a parent that would make a loop.
    pub fn update(&mut self, node: Entity, desc: &UiNodeDesc) -> Result<(), String> {
        checked(ffi::ui_node_update(self.engine.view_mut(), node.index, node.generation, &desc.to_ffi())).map(|_| ())
    }

    /// Destroys a node. With `recursive` everything under it goes too; otherwise its children become roots.
    pub fn destroy(&mut self, node: Entity, recursive: bool) -> Result<(), String> {
        checked(ffi::ui_node_destroy(self.engine.view_mut(), node.index, node.generation, recursive)).map(|_| ())
    }

    pub fn state(&mut self, node: Entity) -> Result<UiNodeState, String> {
        checked(ffi::ui_node_state(self.engine.view_mut(), node.index, node.generation)).map(|o| UiNodeState {
            hovered: o.hovered,
            clicked: o.clicked,
        })
    }

    /// Tells every node tree that something changed which the engine cannot see on its own (edits made through [`UiNodes::update`] need
    /// no call).
    pub fn touch(&mut self) {
        ffi::ui_node_touch();
    }

    /// Spawns a UI tree that lays out the nodes under `root` when one changes, drawn over the primary window at `layer`.
    pub fn spawn_tree(&mut self, root: Entity, name: &str, layer: i32) -> Result<Entity, String> {
        checked(ffi::ui_node_tree_spawn(self.engine.view_mut(), root.index, root.generation, name, layer, 0)).map(|o| entity_of(&o))
    }
}
