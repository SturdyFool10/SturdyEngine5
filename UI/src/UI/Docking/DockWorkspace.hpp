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

// One dockable, tabbed, splittable, resizable panel workspace, drawn/dragged entirely through
// UI::Context's own public API (element()/button()/DragGestureState) — same "not a Clay
// primitive" reasoning as Button.hpp/TextEdit.hpp. Deliberately has zero Platform/Engine/OS
// awareness: dragging a tab out of this workspace's own bounds just raises a
// DockTearOffRequest event (see end_frame()) rather than touching any window API — the
// Engine-level DockWindowCoordinator (Phase 3, Engine/) is what turns that into an actual new OS
// window. This keeps UI/Docking usable from a bare game loop exactly like the rest of UI/, per
// UI.hpp's own doc comment.
//
// Every drag/resize gesture here (tab dragging, split-divider resizing) is built on the same
// UI::DragGestureState every other draggable widget in this package (Slider.hpp, ColorPicker.hpp)
// uses — one shared press/capture/threshold/release state machine, not a docking-specific
// reimplementation.
namespace SFT::UI::Docking {

    struct DockWorkspaceStyle {
        f32 tab_strip_height = 28.0f;
        f32 divider_thickness = 6.0f;
        f32 min_leaf_size = 80.0f;
        // Pixels of pointer movement past a tab's press-down point before it counts as a drag
        // rather than a click — without this, every ordinary tab-select click would also fire a
        // one-pixel reorder/hover-guide flicker. Passed straight through as DragGestureState's own
        // `threshold` for tab gestures; split dividers use 0 (always an immediate drag).
        f32 drag_start_threshold = 4.0f;

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

    // A dragged tab was released outside this workspace's own rect entirely — the panel is still
    // sitting in this workspace's tree (removal is deferred, see end_frame()'s own doc comment) so
    // a failed/declined spawn on the coordinator side doesn't silently lose it.
    struct DockTearOffRequest {
        DockPanelId panel;
        glm::vec2 workspace_local_drop_position{0.0f};
    };

    struct DockWorkspaceEvents {
        vector<DockTearOffRequest> tear_off_requests;
        vector<DockPanelId> close_requests;
    };

    // Public, read-only snapshot for a coordinator while a tab drag is genuinely active (past its
    // threshold). Pointer coordinates are always local to the workspace's own top-left, regardless
    // of where that workspace sits in the Context root.
    struct DockActiveTabDragSnapshot {
        DockPanelId panel;
        DockNodeId source_leaf{};
        glm::vec2 workspace_local_pointer_position{0.0f};
    };

    // One instance per OS window once Phase 3 wires up tear-off (today: usable for a single
    // in-process window too — Phase 1 has no OS-window concept at all yet).
    class DockWorkspace {
      public:
        // `id_prefix` namespaces every Clay id this workspace derives (tab/divider/content ids) so
        // multiple DockWorkspace instances sharing one Context (or, from Phase 3 on, one per OS
        // window with its own Context) never collide on Context::hovered()/clicked() ids.
        explicit DockWorkspace(UString id_prefix, DockWorkspaceStyle style = {})
            : id_prefix_(std::move(id_prefix)), style_(style) {}

        // Registers and places one logical panel as a single operation. Duplicate/empty ids and
        // stale/non-leaf explicit targets are rejected without changing either metadata or tree.
        // `placement == nullopt` docks into the focused leaf, falling back to the first live leaf.
        bool add_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt) {
            return accept_panel(std::move(desc), placement);
        }

        // Transfer destination API: accepts the complete descriptor so title/closability survive a
        // cross-workspace move. The same duplicate and placement validation as add_panel() applies.
        bool accept_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt) {
            const DockPanelId id = desc.id;
            if (id.empty() || panels_.contains(id) || tree_.find_leaf_of(id)) {
                return false;
            }

            const DockPlacement resolved = placement.value_or(
                DockPlacement{.target_node = default_target_leaf(), .zone = DockDropZone::Center});
            if (!is_valid_placement(resolved)) {
                return false;
            }

            panels_.emplace(id, std::move(desc));
            if (!apply_placement(resolved.target_node, resolved.zone, id)) {
                panels_.erase(id);
                return false;
            }
            return true;
        }

        void remove_panel(const DockPanelId &id) {
            if (active_drag_ && active_drag_->kind == ActiveDrag::Kind::Tab &&
                active_drag_->panel == id) {
                active_drag_.reset();
            }
            if (auto drag = tab_drag_.find(id); drag != tab_drag_.end()) {
                drag->second.reset();
            }

            tree_.remove_panel(id);
            panels_.erase(id);
            tab_states_.erase(id);
            tab_close_states_.erase(id);
            tab_drag_.erase(id);

            // Removing the only panel in a subtree may also remove the divider currently being
            // dragged. Clear that business state immediately; Context drops any now-undeclared
            // capture when this frame finishes.
            if (active_drag_ && active_drag_->kind == ActiveDrag::Kind::Divider &&
                tree_.node(active_drag_->resizing_node) == nullptr) {
                if (auto drag = divider_drag_.find(active_drag_->resizing_node);
                    drag != divider_drag_.end()) {
                    drag->second.reset();
                }
                active_drag_.reset();
            }
            if (focused_leaf_ && tree_.node(*focused_leaf_) == nullptr) {
                focused_leaf_ = std::nullopt;
            }
        }

        // Transfer source API. Returns the complete descriptor and removes the panel only when the
        // id is registered; callers can pass the result directly to another accept_panel().
        [[nodiscard]] optional<DockPanelDesc> take_panel(const DockPanelId &id) {
            const auto found = panels_.find(id);
            if (found == panels_.end()) {
                return std::nullopt;
            }
            DockPanelDesc desc = found->second;
            remove_panel(id);
            return desc;
        }

        [[nodiscard]] bool has_panel(const DockPanelId &id) const noexcept { return panels_.contains(id); }

        [[nodiscard]] const DockPanelDesc *panel_desc(const DockPanelId &id) const noexcept {
            const auto found = panels_.find(id);
            return found != panels_.end() ? &found->second : nullptr;
        }

        // Whole tree has zero panels — signals (for a torn-off window) "this window should now be
        // destroyed," since its one purpose no longer has any content.
        [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }

        // Whichever leaf was most recently interacted with (a panel just added with no explicit
        // placement, a tab clicked/dragged, a placement just applied) — nullopt only before this
        // workspace has ever held a panel. Exposed mainly so a caller can build a DockPlacement
        // relative to "wherever the user is currently looking," e.g. a "new panel" button docking
        // next to whatever's focused rather than always at the root.
        [[nodiscard]] optional<DockNodeId> focused_leaf() const noexcept { return focused_leaf_; }

        [[nodiscard]] optional<DockActiveTabDragSnapshot> active_tab_drag() const {
            if (!active_drag_ || active_drag_->kind != ActiveDrag::Kind::Tab) {
                return std::nullopt;
            }
            return DockActiveTabDragSnapshot{
                .panel = active_drag_->panel,
                .source_leaf = active_drag_->source_leaf,
                .workspace_local_pointer_position = active_drag_->workspace_local_pointer_position,
            };
        }

        // One frame: `workspace_rect` is in Context-root coordinates. Layout and all public drag/
        // tear-off positions are workspace-local (origin 0,0); only emitted floating ElementDecls
        // are translated back by workspace_rect.origin. This keeps a nonzero workspace origin from
        // leaking into coordinator-facing coordinates. The caller must already have called
        // ctx.begin_layout(). Call panel_content_region() afterward, then end_frame().
        void begin_frame(Context &ctx, DockRect workspace_rect, f32 delta_seconds) {
            last_delta_seconds_ = delta_seconds;
            workspace_rect_ = workspace_rect;
            last_layout_ = compute_dock_layout(tree_, workspace_local_rect(), style_.tab_strip_height,
                                               style_.divider_thickness);

            update_drag_state(ctx);

            // update_drag_state() may have just mutated tree_ (a drop resolved, a live resize
            // changed a ratio, a same-leaf reorder) — last_layout_ above reflects the *pre*-mutation
            // tree, so it's recomputed before anything this frame actually draws from it. Trees here
            // are a handful of nodes, so the extra pass is not worth avoiding.
            last_layout_ = compute_dock_layout(tree_, workspace_local_rect(), style_.tab_strip_height,
                                               style_.divider_thickness);

            draw_chrome(ctx);

            if (active_drag_ && active_drag_->kind == ActiveDrag::Kind::Tab && active_drag_->hover_placement) {
                draw_drop_guide_for(ctx, *active_drag_->hover_placement);
            }
            if (foreign_drag_hover_) {
                draw_drop_guide_for(ctx, *foreign_drag_hover_);
            }
        }

        // Returns the ready-to-use floating ElementDecl for `id`'s content area, only when `id` is
        // the currently active tab of whichever leaf holds it this frame. Caller wraps its own
        // widget calls: `if (auto d = ws.panel_content_region(id)) { auto scope = ctx.element(*d);
        // ... }`.
        [[nodiscard]] optional<ElementDecl> panel_content_region(const DockPanelId &id) const {
            const optional<DockNodeId> leaf = tree_.find_leaf_of(id);
            if (!leaf) {
                return std::nullopt;
            }
            const DockNode *n = tree_.node(*leaf);
            if (n == nullptr || n->tabs.empty() || n->tabs[n->active_tab_index] != id) {
                return std::nullopt;
            }
            const DockNodeLayout *nl = layout_for(*leaf);
            if (nl == nullptr) {
                return std::nullopt;
            }
            return ElementDecl{
                .sizing = {SizingAxis::fixed(nl->content_rect.size.x), SizingAxis::fixed(nl->content_rect.size.y)},
                .background_color = style_.content_background,
                .clip = {.horizontal = false, .vertical = true},
                // Floating root offsets are Context-root coordinates, so translate the solved
                // workspace-local rect by this workspace's root-space origin.
                .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                             .parent_attach_point = FloatingAttachPoint::LeftTop,
                             .offset = to_context_root(nl->content_rect.origin)},
                .debug_label = UString{"DockPanelContent"},
                .id = UString{id_prefix_.cpp_string() + "##content##" + id.cpp_string()},
            };
        }

        // Finalizes this frame: applies any tab closes (X button clicks) and returns everything
        // that happened this frame. Tear-off requests do *not* remove the panel from this
        // workspace's tree here — the caller (Phase 3's DockWindowCoordinator) owns confirming a
        // new OS window actually got spawned before calling remove_panel() itself, so a failed
        // spawn leaves the panel exactly where it was instead of silently vanishing.
        [[nodiscard]] DockWorkspaceEvents end_frame(Context &ctx) {
            (void)ctx;
            DockWorkspaceEvents events = std::move(pending_events_);
            pending_events_ = DockWorkspaceEvents{};
            for (const DockPanelId &closed : events.close_requests) {
                remove_panel(closed);
            }
            return events;
        }

        // Cross-workspace preview is intentionally non-mutating: only the coordinator owns the
        // descriptor and decides when to call accept_panel(). `local_pointer` is already translated
        // into this workspace's 0,0-based local coordinates. The returned placement remains valid
        // only while the target tree is unchanged.
        [[nodiscard]] optional<DockPlacement> preview_foreign_drag(Context &ctx,
                                                                    glm::vec2 local_pointer) {
            (void)ctx;
            if (!workspace_local_rect().contains(local_pointer)) {
                foreign_drag_hover_.reset();
                return std::nullopt;
            }
            optional<DockPlacement> hit = hit_test_drop_target(local_pointer, std::nullopt);
            if (!hit) {
                const DockPlacement fallback{
                    .target_node = default_target_leaf(),
                    .zone = DockDropZone::Center,
                };
                if (is_valid_placement(fallback)) {
                    hit = fallback;
                }
            }
            foreign_drag_hover_ = hit;
            return hit;
        }

        // Source-compatible compatibility overload. It now reports whether a placement is available;
        // it never inserts `foreign_panel`, even when `released` is true, because doing so without a
        // DockPanelDesc would lose title/closability metadata. Use panel_desc()/take_panel() and
        // accept_panel() to perform the transfer explicitly.
        bool preview_foreign_drag(Context &ctx, const DockPanelId &foreign_panel,
                                  glm::vec2 local_pointer, bool released) {
            (void)foreign_panel;
            (void)released;
            return preview_foreign_drag(ctx, local_pointer).has_value();
        }

        void clear_foreign_drag_preview() noexcept { foreign_drag_hover_.reset(); }

      private:
        // Business identity of whichever single gesture (at most one, since Context's pointer
        // capture is exclusive) is currently in progress — the low-level press/capture/threshold
        // tracking itself lives in the per-id DragGestureState map entries below; this is just
        // "which tab/divider it is and what it means" bookkeeping layered on top.
        struct ActiveDrag {
            enum class Kind : u8 { Tab, Divider } kind;
            DockPanelId panel;          // Kind::Tab.
            DockNodeId source_leaf{};   // Kind::Tab.
            DockNodeId resizing_node{}; // Kind::Divider.
            f32 anchor_ratio = 0.5f;    // Kind::Divider: split_ratio snapshotted when the drag started.
            glm::vec2 workspace_local_pointer_position{0.0f}; // Kind::Tab.
            optional<DockPlacement> hover_placement; // Kind::Tab: this frame's drop candidate.
        };

        [[nodiscard]] DockRect workspace_local_rect() const noexcept {
            return DockRect{.origin = glm::vec2{0.0f}, .size = workspace_rect_.size};
        }

        [[nodiscard]] glm::vec2 to_context_root(glm::vec2 workspace_local) const noexcept {
            return workspace_rect_.origin + workspace_local;
        }

        [[nodiscard]] DragGestureState::UpdateResult to_workspace_local(
            DragGestureState::UpdateResult result) const noexcept {
            result.position -= workspace_rect_.origin;
            return result;
        }

        [[nodiscard]] DockNodeId default_target_leaf() const noexcept {
            if (focused_leaf_) {
                const DockNode *focused = tree_.node(*focused_leaf_);
                if (focused != nullptr && focused->kind == DockNode::Kind::Leaf) {
                    return *focused_leaf_;
                }
            }
            DockNodeId target = tree_.root();
            while (const DockNode *n = tree_.node(target)) {
                if (n->kind == DockNode::Kind::Leaf) {
                    return target;
                }
                target = n->first_child;
            }
            return invalid_dock_node_id;
        }

        [[nodiscard]] bool is_valid_placement(const DockPlacement &placement) const noexcept {
            const DockNode *target = tree_.node(placement.target_node);
            return target != nullptr && target->kind == DockNode::Kind::Leaf;
        }

        bool apply_placement(DockNodeId target, DockDropZone zone, const DockPanelId &panel) {
            bool applied = false;
            switch (zone) {
            case DockDropZone::Center: applied = tree_.merge_into_leaf(target, panel); break;
            case DockDropZone::Left:
                applied = tree_.split_leaf(target, DockSplitAxis::Horizontal,
                                           /*panel_first=*/true, panel);
                break;
            case DockDropZone::Right:
                applied = tree_.split_leaf(target, DockSplitAxis::Horizontal,
                                           /*panel_first=*/false, panel);
                break;
            case DockDropZone::Top:
                applied = tree_.split_leaf(target, DockSplitAxis::Vertical,
                                           /*panel_first=*/true, panel);
                break;
            case DockDropZone::Bottom:
                applied = tree_.split_leaf(target, DockSplitAxis::Vertical,
                                           /*panel_first=*/false, panel);
                break;
            }
            if (applied) {
                focused_leaf_ = tree_.find_leaf_of(panel);
            }
            return applied;
        }

        [[nodiscard]] const DockNodeLayout *layout_for(DockNodeId id) const noexcept {
            for (const DockNodeLayout &nl : last_layout_) {
                if (nl.node == id) {
                    return &nl;
                }
            }
            return nullptr;
        }

        [[nodiscard]] UString tab_id_for(const DockPanelId &panel) const {
            return UString{id_prefix_.cpp_string() + "##tab##" + panel.cpp_string()};
        }

        [[nodiscard]] UString close_button_id_for(const DockPanelId &panel) const {
            return UString{tab_id_for(panel).cpp_string() + "##close"};
        }

        [[nodiscard]] UString divider_id_for(DockNodeId node) const {
            return UString{id_prefix_.cpp_string() + "##divider##" + std::to_string(static_cast<u32>(node))};
        }

        [[nodiscard]] optional<usize> tab_index_under_pointer(Context &ctx, DockNodeId leaf) const {
            const DockNode *n = tree_.node(leaf);
            if (n == nullptr) {
                return std::nullopt;
            }
            for (usize i = 0; i < n->tabs.size(); ++i) {
                if (ctx.hovered(tab_id_for(n->tabs[i]))) {
                    return i;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] static DockDropZone classify_drop_zone(const DockRect &rect, glm::vec2 point) noexcept {
            const f32 u = rect.size.x > 0.0f ? (point.x - rect.origin.x) / rect.size.x : 0.5f;
            const f32 v = rect.size.y > 0.0f ? (point.y - rect.origin.y) / rect.size.y : 0.5f;
            if (u >= 0.25f && u <= 0.75f && v >= 0.25f && v <= 0.75f) {
                return DockDropZone::Center;
            }
            const f32 dist_left = u;
            const f32 dist_right = 1.0f - u;
            const f32 dist_top = v;
            const f32 dist_bottom = 1.0f - v;
            const f32 nearest = std::min({dist_left, dist_right, dist_top, dist_bottom});
            if (nearest == dist_left) {
                return DockDropZone::Left;
            }
            if (nearest == dist_right) {
                return DockDropZone::Right;
            }
            return nearest == dist_top ? DockDropZone::Top : DockDropZone::Bottom;
        }

        // Finds which other leaf's tab strip or content rect (if any) `point` falls over, excluding
        // `exclude` (the dragged tab's own origin leaf, for an in-workspace drag; nullopt for a
        // foreign drag being previewed, which has no leaf of its own in this tree). Landing on a
        // leaf's tab strip is always treated as DockDropZone::Center — a very natural "drop it as a
        // tab here" target, distinct from that leaf's content-rect-based edge/center split zones.
        [[nodiscard]] optional<DockPlacement> hit_test_drop_target(glm::vec2 point, optional<DockNodeId> exclude) const {
            for (const DockNodeLayout &nl : last_layout_) {
                if (exclude && nl.node == *exclude) {
                    continue;
                }
                const DockNode *n = tree_.node(nl.node);
                if (n == nullptr || n->kind != DockNode::Kind::Leaf) {
                    continue;
                }
                if (nl.tab_strip_rect.contains(point)) {
                    return DockPlacement{nl.node, DockDropZone::Center};
                }
                if (nl.content_rect.contains(point)) {
                    return DockPlacement{nl.node, classify_drop_zone(nl.content_rect, point)};
                }
            }
            return std::nullopt;
        }

        void reorder_within_leaf(Context &ctx, const ActiveDrag &active) {
            const DockNode *n = tree_.node(active.source_leaf);
            if (n == nullptr) {
                return;
            }
            const auto it = std::find(n->tabs.begin(), n->tabs.end(), active.panel);
            if (it == n->tabs.end()) {
                return;
            }
            const usize from_index = static_cast<usize>(it - n->tabs.begin());
            const usize to_index = tab_index_under_pointer(ctx, active.source_leaf).value_or(from_index);
            if (to_index != from_index) {
                tree_.reorder_tab(active.source_leaf, from_index, to_index);
            }
        }

        // Called every frame a tab-drag is active (past the threshold, still held): decides between
        // live same-leaf reordering and updating the cross-leaf drop-zone preview.
        void update_tab_drag_hover(Context &ctx, ActiveDrag &active, const DragGestureState::UpdateResult &r) {
            const DockNodeLayout *source_nl = layout_for(active.source_leaf);
            if (source_nl != nullptr && source_nl->tab_strip_rect.contains(r.position)) {
                reorder_within_leaf(ctx, active);
                active.hover_placement.reset();
                return;
            }
            // A leaf with 2+ tabs is a legitimate self-split target (drag one of its own tabs
            // toward its own edge to peel it into a new sibling leaf — the single most common
            // docking gesture, e.g. splitting a tabbed panel into two side-by-side views); a leaf
            // with only the dragged tab in it is not (removing it would leave 0 tabs, the
            // degenerate case DockTree::split_leaf never sees since remove_panel runs first).
            const DockNode *source_node = tree_.node(active.source_leaf);
            const bool source_is_splittable = source_node != nullptr && source_node->tabs.size() > 1;
            active.hover_placement =
                hit_test_drop_target(r.position, source_is_splittable ? std::nullopt : optional<DockNodeId>{active.source_leaf});
        }

        // Called the frame a tab-drag gesture ends (release or cancellation).
        void resolve_tab_drag_end(const ActiveDrag &active, const DragGestureState::UpdateResult &r) {
            if (r.cancelled) {
                return; // interrupted mid-drag - leave the panel exactly where it started.
            }
            if (active.hover_placement) {
                const DockPanelId panel = active.panel;
                tree_.remove_panel(panel);
                apply_placement(active.hover_placement->target_node, active.hover_placement->zone, panel);
                return;
            }
            if (!workspace_local_rect().contains(r.position)) {
                pending_events_.tear_off_requests.push_back(
                    DockTearOffRequest{.panel = active.panel, .workspace_local_drop_position = r.position});
            }
            // Released inside the workspace but over no valid tab-strip/content target (e.g. a gap
            // between leaves) - snap back to where it was, a no-op.
        }

        void apply_divider_delta(const ActiveDrag &active, const DragGestureState::UpdateResult &r) {
            const DockNode *n = tree_.node(active.resizing_node);
            const DockNodeLayout *nl = layout_for(active.resizing_node);
            if (n == nullptr || nl == nullptr) {
                return;
            }
            const bool horizontal = n->split_axis == DockSplitAxis::Horizontal;
            const f32 extent = horizontal ? nl->full_rect.size.x : nl->full_rect.size.y;
            const f32 delta = horizontal ? r.delta_since_start.x : r.delta_since_start.y;
            const f32 delta_ratio = extent > 1.0f ? delta / extent : 0.0f;
            const f32 min_ratio = extent > 0.0f ? std::clamp(style_.min_leaf_size / extent, 0.0f, 0.5f) : 0.0f;
            tree_.set_split_ratio(active.resizing_node, std::clamp(active.anchor_ratio + delta_ratio, min_ratio, 1.0f - min_ratio));
        }

        // Drives whichever gesture is already in progress, if any — otherwise scans every tab/
        // divider this frame for a fresh press. At most one DragGestureState across either map can
        // ever be capturing at a time (Context's pointer capture is exclusive), so re-scanning every
        // candidate on frames where nothing has started yet is cheap and correct: every id besides
        // the one actually pressed just no-ops.
        void update_drag_state(Context &ctx) {
            if (active_drag_) {
                continue_active_drag(ctx);
                return;
            }
            scan_for_new_drag(ctx);
        }

        void continue_active_drag(Context &ctx) {
            ActiveDrag &active = *active_drag_;
            if (active.kind == ActiveDrag::Kind::Divider) {
                const DragGestureState::UpdateResult r = to_workspace_local(
                    divider_drag_[active.resizing_node].update(
                        ctx, divider_id_for(active.resizing_node), 0.0f));
                if (r.active) {
                    apply_divider_delta(active, r);
                }
                if (r.ended) {
                    active_drag_.reset();
                }
                return;
            }
            const DragGestureState::UpdateResult r = to_workspace_local(
                tab_drag_[active.panel].update(
                    ctx, tab_id_for(active.panel), style_.drag_start_threshold));
            active.workspace_local_pointer_position = r.position;
            if (r.active) {
                update_tab_drag_hover(ctx, active, r);
            }
            if (r.ended) {
                resolve_tab_drag_end(active, r);
                active_drag_.reset();
            }
        }

        void scan_for_new_drag(Context &ctx) {
            for (const DockNodeLayout &nl : last_layout_) {
                const DockNode *n = tree_.node(nl.node);
                if (n == nullptr) {
                    continue;
                }
                if (n->kind == DockNode::Kind::Split) {
                    const DragGestureState::UpdateResult r = to_workspace_local(
                        divider_drag_[nl.node].update(ctx, divider_id_for(nl.node), 0.0f));
                    if (r.started) {
                        active_drag_ = ActiveDrag{.kind = ActiveDrag::Kind::Divider, .resizing_node = nl.node, .anchor_ratio = n->split_ratio};
                        return;
                    }
                    continue;
                }
                for (const DockPanelId &tab : n->tabs) {
                    // The close button is a child nested inside the tab's own hit box, so a click on
                    // it also satisfies clicked(tab_id) — skipped first so closing a tab can't also
                    // kick off a tab-drag for a panel close_requests is about to remove this frame.
                    if (ctx.clicked(close_button_id_for(tab))) {
                        continue;
                    }
                    const DragGestureState::UpdateResult r = to_workspace_local(
                        tab_drag_[tab].update(ctx, tab_id_for(tab), style_.drag_start_threshold));
                    if (r.ended && !r.committed && !r.cancelled) {
                        // Pressed and released without ever crossing the threshold - a plain click.
                        tree_.set_active_tab(nl.node, tab);
                        focused_leaf_ = nl.node;
                        continue;
                    }
                    if (r.started) {
                        active_drag_ = ActiveDrag{
                            .kind = ActiveDrag::Kind::Tab,
                            .panel = tab,
                            .source_leaf = nl.node,
                            .workspace_local_pointer_position = r.position,
                        };
                        focused_leaf_ = nl.node;
                        return;
                    }
                }
            }
        }

        void draw_leaf_chrome(Context &ctx, const DockNodeLayout &nl, const DockNode &n) {
            auto strip_scope = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(nl.tab_strip_rect.size.x), SizingAxis::fixed(nl.tab_strip_rect.size.y)},
                .direction = LayoutDirection::LeftToRight,
                .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                             .parent_attach_point = FloatingAttachPoint::LeftTop,
                             .offset = to_context_root(nl.tab_strip_rect.origin)},
                .debug_label = UString{"DockTabStrip"},
            });
            (void)strip_scope;

            for (usize i = 0; i < n.tabs.size(); ++i) {
                const DockPanelId &panel_id = n.tabs[i];
                const bool active = i == n.active_tab_index;
                const auto desc_it = panels_.find(panel_id);
                const UString title = desc_it != panels_.end() ? desc_it->second.title : panel_id;
                const bool closable = desc_it != panels_.end() && desc_it->second.closable;
                const UString tid = tab_id_for(panel_id);

                ButtonResult tab_result = button(
                    ctx,
                    ElementDecl{
                        .sizing = {SizingAxis::fit(48.0f), SizingAxis::fixed(nl.tab_strip_rect.size.y)},
                        .padding = Padding::symmetric(10, 4),
                        .child_alignment = {.x = AlignX::Center, .y = AlignY::Center},
                        .id = tid,
                    },
                    active ? style_.tab_active_style : style_.tab_inactive_style, tab_states_[panel_id], last_delta_seconds_);
                (void)tab_result;
                ctx.text(title.as_ustr(), TextStyle{.color = style_.tab_text_color, .font_id = style_.tab_font_id, .font_size = style_.tab_font_size});

                if (closable) {
                    ButtonResult close_result = button(
                        ctx,
                        ElementDecl{
                            .sizing = {SizingAxis::fixed(16.0f), SizingAxis::fixed(16.0f)},
                            .child_alignment = {.x = AlignX::Center, .y = AlignY::Center},
                            .corner_radius = CornerRadius::all(3.0f),
                            .id = close_button_id_for(panel_id),
                        },
                        style_.close_button_style, tab_close_states_[panel_id], last_delta_seconds_);
                    const UString close_label{"x"};
                    ctx.text(close_label.as_ustr(), TextStyle{.color = style_.tab_text_color, .font_id = style_.tab_font_id, .font_size = style_.tab_font_size});
                    if (close_result.clicked) {
                        pending_events_.close_requests.push_back(panel_id);
                    }
                }
            }
        }

        void draw_divider(Context &ctx, const DockNodeLayout &nl, DockSplitAxis axis) {
            ButtonResult result = button(
                ctx,
                ElementDecl{
                    .sizing = {SizingAxis::fixed(nl.divider_rect.size.x), SizingAxis::fixed(nl.divider_rect.size.y)},
                    .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                                 .parent_attach_point = FloatingAttachPoint::LeftTop,
                                 .offset = to_context_root(nl.divider_rect.origin)},
                    // A resize handle, not just "something clickable" — overrides button()'s own
                    // Pointer default (see DockSplitAxis's own doc comment, DockTypes.hpp, for which
                    // way each axis actually drags).
                    .cursor = axis == DockSplitAxis::Horizontal ? CursorIcon::ResizeHorizontal : CursorIcon::ResizeVertical,
                    .debug_label = UString{"DockDivider"},
                    .id = divider_id_for(nl.node),
                },
                style_.divider_style, divider_states_[nl.node], last_delta_seconds_);
            (void)result;
        }

        void draw_chrome(Context &ctx) {
            for (const DockNodeLayout &nl : last_layout_) {
                const DockNode *n = tree_.node(nl.node);
                if (n == nullptr) {
                    continue;
                }
                if (n->kind == DockNode::Kind::Leaf) {
                    draw_leaf_chrome(ctx, nl, *n);
                } else {
                    draw_divider(ctx, nl, n->split_axis);
                }
            }
        }

        void draw_drop_guide_for(Context &ctx, const DockPlacement &placement) {
            const DockNodeLayout *nl = layout_for(placement.target_node);
            if (nl == nullptr) {
                return;
            }
            DockRect guide = nl->content_rect;
            switch (placement.zone) {
            case DockDropZone::Center: break;
            case DockDropZone::Left: guide.size.x *= 0.5f; break;
            case DockDropZone::Right: guide.origin.x += guide.size.x * 0.5f; guide.size.x *= 0.5f; break;
            case DockDropZone::Top: guide.size.y *= 0.5f; break;
            case DockDropZone::Bottom: guide.origin.y += guide.size.y * 0.5f; guide.size.y *= 0.5f; break;
            }
            auto scope = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fixed(guide.size.x), SizingAxis::fixed(guide.size.y)},
                .background_color = style_.drop_guide_fill,
                .border = {.color = style_.drop_guide_border, .width = BorderWidth::all(2)},
                // capture_pointer=false: this overlay must not itself become part of next frame's
                // hit-testable committed layout, or it would shadow whatever's underneath it.
                .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                             .parent_attach_point = FloatingAttachPoint::LeftTop,
                             .offset = to_context_root(guide.origin), .z_index = 1000,
                             .capture_pointer = false},
                .debug_label = UString{"DockDropGuide"},
            });
            (void)scope;
        }

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
