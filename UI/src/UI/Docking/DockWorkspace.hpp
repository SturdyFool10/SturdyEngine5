#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>
#pragma endregion

#include "../Button.hpp"
#include "../Context.hpp"
#include "../DragGesture.hpp"
#include "../Style.hpp"
#include "DockLayout.hpp"
#include "DockTypes.hpp"

using std::optional;
using std::unordered_map;
using std::vector;

/// One dockable, tabbed, splittable, resizable panel workspace, drawn/dragged entirely through
/// UI::Context's own public API (element()/button()/DragGestureState) — same "not a Clay
/// primitive" reasoning as Button.hpp/TextEdit.hpp. Deliberately has zero Platform/Engine/OS
/// awareness: dragging a tab out of this workspace's own bounds just raises a
/// DockTearOffRequest event (see end_frame()) rather than touching any window API — the
/// Engine-level DockWindowCoordinator (Phase 3, Engine/) is what turns that into an actual new OS
/// window. This keeps UI/Docking usable from a bare game loop exactly like the rest of UI/, per
/// UI.hpp's own doc comment.
///
/// Every drag/resize gesture here (tab dragging, split-divider resizing) is built on the same
/// UI::DragGestureState every other draggable widget in this package (Slider.hpp, ColorPicker.hpp)
/// uses — one shared press/capture/threshold/release state machine, not a docking-specific
/// reimplementation.
namespace SFT::UI::Docking {

    struct DockWorkspaceStyle {
        f32 tab_strip_height = 28.0f;
        f32 divider_thickness = 6.0f;
        f32 min_leaf_size = 80.0f;
        /// Pixels of pointer movement past a tab's press-down point before it counts as a drag
        /// rather than a click — without this, every ordinary tab-select click would also fire a
        /// one-pixel reorder/hover-guide flicker. Passed straight through as DragGestureState's own
        /// `threshold` for tab gestures; split dividers use 0 (always an immediate drag).
        f32 drag_start_threshold = 4.0f;

        /// Public cursor policy for the workspace's internally generated chrome. These are explicit
        /// values rather than Auto because DockWorkspace owns the declarations; consumers can replace
        /// any of them to match a host application's conventions.
        CursorIcon tab_cursor = CursorIcon::Grab;
        CursorIcon tab_dragging_cursor = CursorIcon::Grabbing;
        CursorIcon close_button_cursor = CursorIcon::Pointer;
        CursorIcon horizontal_divider_cursor = CursorIcon::ResizeHorizontal;
        CursorIcon vertical_divider_cursor = CursorIcon::ResizeVertical;

        ButtonStyle tab_active_style{
            .idle = Color{0.20, 0.22, 0.28, 1.0}, .hovered = Color{0.24, 0.26, 0.33, 1.0}, .pressed = Color{0.20, 0.22, 0.28, 1.0}};
        ButtonStyle tab_inactive_style{
            .idle = Color{0.12, 0.13, 0.16, 1.0}, .hovered = Color{0.17, 0.18, 0.23, 1.0}, .pressed = Color{0.20, 0.22, 0.28, 1.0}};
        ButtonStyle close_button_style{
            .idle = Color{0.0, 0.0, 0.0, 0.0}, .hovered = Color{0.55, 0.18, 0.18, 1.0}, .pressed = Color{0.4, 0.12, 0.12, 1.0}};
        ButtonStyle divider_style{
            .idle = Color{0.15, 0.16, 0.20, 1.0}, .hovered = Color{0.30, 0.50, 0.85, 1.0}, .pressed = Color{0.30, 0.50, 0.85, 1.0},
            .transition_seconds = 0.12f};

        Color content_background{0.10, 0.11, 0.14, 1.0};
        Color drop_guide_fill{0.30, 0.50, 0.85, 0.35};
        Color drop_guide_border{0.45, 0.65, 0.95, 0.9};
        Color tab_text_color{0.92, 0.93, 0.95, 1.0};
        FontId tab_font_id = 0;
        u16 tab_font_size = 14;
    };

    struct DockPlacement {
        DockNodeId target_node{};
        DockDropZone zone = DockDropZone::Center;
    };

    /// A dragged tab was released outside this workspace's own rect entirely — the panel is still
    /// sitting in this workspace's tree (removal is deferred, see end_frame()'s own doc comment) so
    /// a failed/declined spawn on the coordinator side doesn't silently lose it.
    struct DockTearOffRequest {
        DockPanelId panel;
        glm::vec2 workspace_local_drop_position{0.0f};
    };

    struct DockWorkspaceEvents {
        vector<DockTearOffRequest> tear_off_requests;
        vector<DockPanelId> close_requests;
    };

    /// Public, read-only snapshot for a coordinator while a tab drag is genuinely active (past its
    /// threshold). Pointer coordinates are always local to the workspace's own top-left, regardless
    /// of where that workspace sits in the Context root.
    struct DockActiveTabDragSnapshot {
        DockPanelId panel;
        DockNodeId source_leaf{};
        glm::vec2 workspace_local_pointer_position{0.0f};
    };

    /// One instance per OS window once Phase 3 wires up tear-off (today: usable for a single
    /// in-process window too — Phase 1 has no OS-window concept at all yet).
    class DockWorkspace {
      public:
        /// `id_prefix` namespaces every Clay id this workspace derives (tab/divider/content ids) so
        /// multiple DockWorkspace instances sharing one Context (or, from Phase 3 on, one per OS
        /// window with its own Context) never collide on Context::hovered()/clicked() ids.
        explicit DockWorkspace(UString id_prefix, DockWorkspaceStyle style = {});

        void set_content_background(Color color) noexcept;

        /// Registers and places one logical panel as a single operation. Duplicate/empty ids and
        /// stale/non-leaf explicit targets are rejected without changing either metadata or tree.
        /// `placement == nullopt` docks into the focused leaf, falling back to the first live leaf.
        bool add_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt);

        /// Transfer destination API: accepts the complete descriptor so title/closability survive a
        /// cross-workspace move. The same duplicate and placement validation as add_panel() applies.
        bool accept_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt);

        void remove_panel(const DockPanelId &id);

        /// Transfer source API. Returns the complete descriptor and removes the panel only when the
        /// id is registered; callers can pass the result directly to another accept_panel().
        [[nodiscard]] optional<DockPanelDesc> take_panel(const DockPanelId &id);

        [[nodiscard]] bool has_panel(const DockPanelId &id) const noexcept;

        [[nodiscard]] const DockPanelDesc *panel_desc(const DockPanelId &id) const noexcept;

        /// Whole tree has zero panels — signals (for a torn-off window) "this window should now be
        /// destroyed," since its one purpose no longer has any content.
        [[nodiscard]] bool empty() const noexcept;

        /// Whichever leaf was most recently interacted with (a panel just added with no explicit
        /// placement, a tab clicked/dragged, a placement just applied) — nullopt only before this
        /// workspace has ever held a panel. Exposed mainly so a caller can build a DockPlacement
        /// relative to "wherever the user is currently looking," e.g. a "new panel" button docking
        /// next to whatever's focused rather than always at the root.
        [[nodiscard]] optional<DockNodeId> focused_leaf() const noexcept;

        [[nodiscard]] optional<DockActiveTabDragSnapshot> active_tab_drag() const;

        /// One frame: `workspace_rect` is in Context-root coordinates. Layout and all public drag/
        /// tear-off positions are workspace-local (origin 0,0); only emitted floating ElementDecls
        /// are translated back by workspace_rect.origin. This keeps a nonzero workspace origin from
        /// leaking into coordinator-facing coordinates. The caller must already have called
        /// ctx.begin_layout(). Call panel_content_region() afterward, then end_frame().
        void begin_frame(Context &ctx, DockRect workspace_rect, f32 delta_seconds);

        /// Returns the ready-to-use floating ElementDecl for `id`'s content area, only when `id` is
        /// the currently active tab of whichever leaf holds it this frame. Caller wraps its own
        /// widget calls: `if (auto d = ws.panel_content_region(id)) { auto scope = ctx.element(*d);
        /// ... }`.
        [[nodiscard]] optional<ElementDecl> panel_content_region(const DockPanelId &id) const;

        /// Finalizes this frame: applies any tab closes (X button clicks) and returns everything
        /// that happened this frame. Tear-off requests do *not* remove the panel from this
        /// workspace's tree here — the caller (Phase 3's DockWindowCoordinator) owns confirming a
        /// new OS window actually got spawned before calling remove_panel() itself, so a failed
        /// spawn leaves the panel exactly where it was instead of silently vanishing.
        [[nodiscard]] DockWorkspaceEvents end_frame(Context &ctx);

        /// Cross-workspace preview is intentionally non-mutating: only the coordinator owns the
        /// descriptor and decides when to call accept_panel(). `local_pointer` is already translated
        /// into this workspace's 0,0-based local coordinates. The returned placement remains valid
        /// only while the target tree is unchanged.
        [[nodiscard]] optional<DockPlacement> preview_foreign_drag(Context &ctx,
                                                                    glm::vec2 local_pointer);

        /// Source-compatible compatibility overload. It now reports whether a placement is available;
        /// it never inserts `foreign_panel`, even when `released` is true, because doing so without a
        /// DockPanelDesc would lose title/closability metadata. Use panel_desc()/take_panel() and
        /// accept_panel() to perform the transfer explicitly.
        bool preview_foreign_drag(Context &ctx, const DockPanelId &foreign_panel,
                                  glm::vec2 local_pointer, bool released);

        void clear_foreign_drag_preview() noexcept;

      private:
        /// Business identity of whichever single gesture (at most one, since Context's pointer
        /// capture is exclusive) is currently in progress — the low-level press/capture/threshold
        /// tracking itself lives in the per-id DragGestureState map entries below; this is just
        /// "which tab/divider it is and what it means" bookkeeping layered on top.
        struct ActiveDrag {
            enum class Kind : u8 { Tab, Divider } kind;
            DockPanelId panel;
            DockNodeId source_leaf{};
            DockNodeId resizing_node{};
            f32 anchor_ratio = 0.5f;
            glm::vec2 workspace_local_pointer_position{0.0f};
            optional<DockPlacement> hover_placement;
        };

        [[nodiscard]] DockRect workspace_local_rect() const noexcept;

        [[nodiscard]] glm::vec2 to_context_root(glm::vec2 workspace_local) const noexcept;

        [[nodiscard]] DragGestureState::UpdateResult to_workspace_local(
            DragGestureState::UpdateResult result) const noexcept;

        [[nodiscard]] DockNodeId default_target_leaf() const noexcept;

        [[nodiscard]] bool is_valid_placement(const DockPlacement &placement) const noexcept;

        bool apply_placement(DockNodeId target, DockDropZone zone, const DockPanelId &panel);

        [[nodiscard]] const DockNodeLayout *layout_for(DockNodeId id) const noexcept;

        [[nodiscard]] UString tab_id_for(const DockPanelId &panel) const;

        [[nodiscard]] UString close_button_id_for(const DockPanelId &panel) const;

        [[nodiscard]] UString divider_id_for(DockNodeId node) const;

        [[nodiscard]] optional<usize> tab_index_under_pointer(Context &ctx, DockNodeId leaf) const;

        [[nodiscard]] static DockDropZone classify_drop_zone(const DockRect &rect, glm::vec2 point) noexcept;

        /// Finds which other leaf's tab strip or content rect (if any) `point` falls over, excluding
        /// `exclude` (the dragged tab's own origin leaf, for an in-workspace drag; nullopt for a
        /// foreign drag being previewed, which has no leaf of its own in this tree). Landing on a
        /// leaf's tab strip is always treated as DockDropZone::Center — a very natural "drop it as a
        /// tab here" target, distinct from that leaf's content-rect-based edge/center split zones.
        [[nodiscard]] optional<DockPlacement> hit_test_drop_target(glm::vec2 point, optional<DockNodeId> exclude) const;

        void reorder_within_leaf(Context &ctx, const ActiveDrag &active);

        /// Called every frame a tab-drag is active (past the threshold, still held): decides between
        /// live same-leaf reordering and updating the cross-leaf drop-zone preview.
        void update_tab_drag_hover(Context &ctx, ActiveDrag &active, const DragGestureState::UpdateResult &r);

        /// Called the frame a tab-drag gesture ends (release or cancellation).
        void resolve_tab_drag_end(const ActiveDrag &active, const DragGestureState::UpdateResult &r);

        void apply_divider_delta(const ActiveDrag &active, const DragGestureState::UpdateResult &r);

        /// Drives whichever gesture is already in progress, if any — otherwise scans every tab/
        /// divider this frame for a fresh press. At most one DragGestureState across either map can
        /// ever be capturing at a time (Context's pointer capture is exclusive), so re-scanning every
        /// candidate on frames where nothing has started yet is cheap and correct: every id besides
        /// the one actually pressed just no-ops.
        void update_drag_state(Context &ctx);

        void continue_active_drag(Context &ctx);

        void scan_for_new_drag(Context &ctx);

        void draw_leaf_chrome(Context &ctx, const DockNodeLayout &nl, const DockNode &n);

        void draw_divider(Context &ctx, const DockNodeLayout &nl, DockSplitAxis axis);

        void draw_chrome(Context &ctx);

        void draw_drop_guide_for(Context &ctx, const DockPlacement &placement);

        UString id_prefix_;
        DockWorkspaceStyle style_;
        DockTree tree_;
        unordered_map<DockPanelId, DockPanelDesc> panels_;
        unordered_map<DockPanelId, ButtonState> tab_states_;
        unordered_map<DockPanelId, ButtonState> tab_close_states_;
        unordered_map<DockNodeId, ButtonState> divider_states_;
        unordered_map<DockPanelId, DragGestureState> tab_drag_;
        unordered_map<DockNodeId, DragGestureState> divider_drag_;
        vector<DockNodeLayout> last_layout_;
        DockRect workspace_rect_{};
        optional<DockNodeId> focused_leaf_;
        optional<ActiveDrag> active_drag_;
        optional<DockPlacement> foreign_drag_hover_;
        DockWorkspaceEvents pending_events_{};
        f32 last_delta_seconds_ = 0.0f;
    };

} // namespace SFT::UI::Docking
