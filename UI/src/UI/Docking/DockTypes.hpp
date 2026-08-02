#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <glm/vec2.hpp>
#include <optional>
#include <vector>
#pragma endregion

using std::optional;
using std::vector;

// Dock-tree data model: a node-pool tree of Split/Leaf nodes plus the mutating operations a
// docking workspace needs (merge a panel into a tab group, split a leaf, remove a panel and
// collapse the tree around the gap). Pure data + tree algebra — no Context/Clay/rendering
// dependency at all, the same "usable standalone" split Style.hpp keeps from Context.hpp. See
// DockWorkspace.hpp for the Context-facing, per-frame-drawn/dragged layer built on top of this.
namespace SFT::UI::Docking {

    // Stable, app-assigned logical panel identity — a UString (not an opaque numeric handle) so it
    // round-trips directly into DockLayoutDescriptor (Phase 5) and matches ElementDecl::id's own
    // string-keyed stable-identity convention (Style.hpp).
    using DockPanelId = UString;

    // Handle into DockTree's own node pool. Never persisted — DockLayoutDescriptor (Phase 5)
    // serializes the tree's *shape*, not pool indices, since a restored tree gets a fresh pool.
    enum class DockNodeId : u32 {};
    inline constexpr DockNodeId invalid_dock_node_id = static_cast<DockNodeId>(~u32{0});

    enum class DockSplitAxis : u8 {
        Horizontal, // children placed side-by-side; the divider is a vertical line, dragged left/right.
        Vertical,   // children stacked top/bottom; the divider is a horizontal line, dragged up/down.
    };

    enum class DockDropZone : u8 { Center, Left, Right, Top, Bottom };

    // Plain float rect in an explicitly chosen pixel coordinate space. DockLayout preserves the
    // coordinate space supplied by its caller; DockWorkspace solves layouts in workspace-local
    // space and translates only when emitting Context-root floating elements. Kept float, not
    // RHI::Rect2D's int, since split-ratio math wants ElementDecl::Sizing's precision.
    struct DockRect {
        glm::vec2 origin{0.0f};
        glm::vec2 size{0.0f};

        [[nodiscard]] bool contains(glm::vec2 point) const noexcept {
            return point.x >= origin.x && point.x <= origin.x + size.x && point.y >= origin.y &&
                   point.y <= origin.y + size.y;
        }
    };

    struct DockNode {
        enum class Kind : u8 { Split, Leaf } kind = Kind::Leaf;

        // Kind::Split:
        DockSplitAxis split_axis = DockSplitAxis::Horizontal;
        f32 split_ratio = 0.5f; // fraction of the node's size given to first_child; second_child gets the rest.
        DockNodeId first_child = invalid_dock_node_id;
        DockNodeId second_child = invalid_dock_node_id;

        // Kind::Leaf: an ordered tab strip. Empty only transiently — DockTree::remove_panel prunes
        // an emptied non-root leaf by collapsing its parent split away (see that method's own
        // comment); an empty *root* leaf is how DockTree::empty() reports "nothing left to show."
        vector<DockPanelId> tabs;
        usize active_tab_index = 0;
    };

    struct DockPanelDesc {
        DockPanelId id;
        UString title;
        bool closable = true;
    };

    // Node-pool tree: one root plus however many Split/Leaf descendants. One DockTree per
    // DockWorkspace (== per OS window once Phase 3 wires up tear-off — see that phase's own design
    // note on why the two are 1:1).
    class DockTree {
      public:
        DockTree() { nodes_.push_back(DockNode{}); }

        [[nodiscard]] DockNodeId root() const noexcept { return root_; }

        [[nodiscard]] const DockNode *node(DockNodeId id) const noexcept {
            const u32 index = static_cast<u32>(id);
            return index < nodes_.size() && nodes_[index].has_value() ? &*nodes_[index] : nullptr;
        }

        // Docks `panel` as a new tab in leaf `target`, at the end of its strip (or a specific index
        // if `before` is set) and makes it the active tab. `panel` must not currently be anywhere
        // else in this tree — callers that might be moving an already-placed panel must
        // remove_panel() it first (see DockWorkspace's own drop-resolution sequencing for why that
        // order is also what keeps `target`'s DockNodeId itself from being invalidated by the
        // removal, when target is a *different* leaf than the one being vacated).
        bool merge_into_leaf(DockNodeId target, DockPanelId panel,
                                           optional<usize> before = std::nullopt) {
            DockNode *n = mutable_node(target);
            if (!n || n->kind != DockNode::Kind::Leaf || find_leaf_of(panel)) {
                return false;
            }
            const usize insert_at = before && *before <= n->tabs.size() ? *before : n->tabs.size();
            n->tabs.insert(n->tabs.begin() + static_cast<isize>(insert_at), panel);
            n->active_tab_index = insert_at;
            return true;
        }

        // Splits leaf `target` along `axis`: `target` itself becomes a Split node with two new leaf
        // children — one keeping `target`'s existing tabs, the other holding only `panel`.
        // `panel_first` picks which child gets the new panel (true = first_child, i.e. left/top of
        // the split — see DockSplitAxis's own doc comment for what "first" means per axis).
        bool split_leaf(DockNodeId target, DockSplitAxis axis, bool panel_first,
                                      DockPanelId panel, f32 new_panel_ratio = 0.25f) {
            DockNode *target_before = mutable_node(target);
            if (!target_before || target_before->kind != DockNode::Kind::Leaf || find_leaf_of(panel)) {
                return false;
            }
            DockNode existing_leaf{};
            existing_leaf.kind = DockNode::Kind::Leaf;
            existing_leaf.tabs = std::move(target_before->tabs);
            existing_leaf.active_tab_index = target_before->active_tab_index;

            DockNode new_leaf{};
            new_leaf.kind = DockNode::Kind::Leaf;
            new_leaf.tabs = {std::move(panel)};
            new_leaf.active_tab_index = 0;

            // alloc_node() below may reallocate nodes_ (vector::push_back), which would invalidate
            // target_before — it's already been fully read (moved out of) above, and target is
            // re-fetched fresh via mutable_node() after both allocations, so no dangling access.
            const DockNodeId existing_id = alloc_node(std::move(existing_leaf));
            const DockNodeId new_id = alloc_node(std::move(new_leaf));

            DockNode *target_after = mutable_node(target);
            target_after->kind = DockNode::Kind::Split;
            target_after->split_axis = axis;
            const f32 clamped_new_panel_ratio = std::clamp(new_panel_ratio, 0.0f, 1.0f);
            target_after->split_ratio =
                panel_first ? clamped_new_panel_ratio : (1.0f - clamped_new_panel_ratio);
            target_after->first_child = panel_first ? new_id : existing_id;
            target_after->second_child = panel_first ? existing_id : new_id;
            target_after->tabs.clear();
            return true;
        }

        // Removes `panel` from whichever leaf holds it. If that leaf becomes empty and isn't the
        // root, the leaf and its now-redundant parent Split are both freed and the parent's slot
        // (wherever it was referenced from) is replaced by the *sibling* — standard dock-tree
        // pruning, mirroring ImGui's own DockNode collapse behavior. No-op if `panel` isn't in the
        // tree at all (safe to call speculatively).
        void remove_panel(const DockPanelId &panel) {
            const optional<DockNodeId> leaf_id = find_leaf_of(panel);
            if (!leaf_id) {
                return;
            }
            DockNode *leaf = mutable_node(*leaf_id);
            const auto it = std::find(leaf->tabs.begin(), leaf->tabs.end(), panel);
            if (it == leaf->tabs.end()) {
                return;
            }
            const usize removed_index = static_cast<usize>(it - leaf->tabs.begin());
            leaf->tabs.erase(it);
            if (leaf->tabs.empty()) {
                leaf->active_tab_index = 0;
            } else if (removed_index < leaf->active_tab_index) {
                leaf->active_tab_index -= 1;
            } else if (leaf->active_tab_index >= leaf->tabs.size()) {
                leaf->active_tab_index = leaf->tabs.size() - 1;
            }
            if (!leaf->tabs.empty() || *leaf_id == root()) {
                return; // still has tabs, or is the (allowed-empty) root leaf — nothing to collapse.
            }
            collapse_empty_leaf(*leaf_id);
        }

        [[nodiscard]] optional<DockNodeId> find_leaf_of(const DockPanelId &panel) const noexcept {
            return find_leaf_of_from(panel, root());
        }

        // True only when the root itself is an empty leaf — the tree has no panels left at all
        // (DockWorkspace::empty() surfaces this to decide whether a torn-off window should close).
        [[nodiscard]] bool empty() const noexcept {
            const DockNode *r = node(root());
            return r != nullptr && r->kind == DockNode::Kind::Leaf && r->tabs.empty();
        }

        void set_split_ratio(DockNodeId id, f32 ratio) noexcept {
            DockNode *n = mutable_node(id);
            if (n != nullptr && n->kind == DockNode::Kind::Split) {
                n->split_ratio = std::clamp(ratio, 0.0f, 1.0f);
            }
        }

        void set_active_tab(DockNodeId leaf, const DockPanelId &panel) noexcept {
            DockNode *n = mutable_node(leaf);
            if (n == nullptr || n->kind != DockNode::Kind::Leaf) {
                return;
            }
            for (usize i = 0; i < n->tabs.size(); ++i) {
                if (n->tabs[i] == panel) {
                    n->active_tab_index = i;
                    return;
                }
            }
        }

        void reorder_tab(DockNodeId leaf, usize from_index, usize to_index) noexcept {
            DockNode *n = mutable_node(leaf);
            if (n == nullptr || n->kind != DockNode::Kind::Leaf) {
                return;
            }
            if (from_index >= n->tabs.size() || to_index >= n->tabs.size() || from_index == to_index) {
                return;
            }
            const DockPanelId moved = n->tabs[from_index];
            n->tabs.erase(n->tabs.begin() + static_cast<isize>(from_index));
            n->tabs.insert(n->tabs.begin() + static_cast<isize>(to_index), moved);
            n->active_tab_index = to_index;
        }

      private:
        [[nodiscard]] DockNode *mutable_node(DockNodeId id) noexcept {
            const u32 index = static_cast<u32>(id);
            return index < nodes_.size() && nodes_[index].has_value() ? &*nodes_[index] : nullptr;
        }

        [[nodiscard]] DockNodeId alloc_node(DockNode value) {
            if (!free_list_.empty()) {
                const u32 index = free_list_.back();
                free_list_.pop_back();
                nodes_[index] = std::move(value);
                return static_cast<DockNodeId>(index);
            }
            nodes_.push_back(std::move(value));
            return static_cast<DockNodeId>(nodes_.size() - 1);
        }

        void free_node(DockNodeId id) {
            const u32 index = static_cast<u32>(id);
            if (index < nodes_.size()) {
                nodes_[index].reset();
                free_list_.push_back(index);
            }
        }

        [[nodiscard]] optional<DockNodeId> find_leaf_of_from(const DockPanelId &panel, DockNodeId from) const noexcept {
            const DockNode *n = node(from);
            if (n == nullptr) {
                return std::nullopt;
            }
            if (n->kind == DockNode::Kind::Leaf) {
                for (const DockPanelId &tab : n->tabs) {
                    if (tab == panel) {
                        return from;
                    }
                }
                return std::nullopt;
            }
            if (const optional<DockNodeId> first = find_leaf_of_from(panel, n->first_child)) {
                return first;
            }
            return find_leaf_of_from(panel, n->second_child);
        }

        struct ParentLink {
            DockNodeId parent{};
            bool is_first = false;
        };

        // Searches downward from `from` for the Split node whose first_child/second_child ==
        // `child` — nodes don't store a parent pointer of their own (kept a strict tree, no back-
        // edges), so collapse_empty_leaf() looks it up on the rare occasion (a panel closing/tearing
        // off) that it actually needs to relink one.
        [[nodiscard]] optional<ParentLink> find_parent(DockNodeId child, DockNodeId from) const noexcept {
            const DockNode *n = node(from);
            if (n == nullptr || n->kind != DockNode::Kind::Split) {
                return std::nullopt;
            }
            if (n->first_child == child) {
                return ParentLink{from, true};
            }
            if (n->second_child == child) {
                return ParentLink{from, false};
            }
            if (const optional<ParentLink> first = find_parent(child, n->first_child)) {
                return first;
            }
            return find_parent(child, n->second_child);
        }

        void collapse_empty_leaf(DockNodeId leaf_id) {
            const optional<ParentLink> link = find_parent(leaf_id, root());
            if (!link) {
                return; // leaf_id == root(), already handled by the caller.
            }
            const DockNode *parent = node(link->parent);
            const DockNodeId sibling = link->is_first ? parent->second_child : parent->first_child;
            const optional<ParentLink> grandparent = find_parent(link->parent, root());

            free_node(leaf_id);
            free_node(link->parent);

            if (grandparent) {
                DockNode *gp = mutable_node(grandparent->parent);
                (grandparent->is_first ? gp->first_child : gp->second_child) = sibling;
            } else {
                // link->parent had no parent of its own, i.e. it *was* the root — the surviving
                // sibling becomes the new root directly (root_ is just a handle, so this is a
                // cheap repoint, not a move of any node's storage).
                root_ = sibling;
            }
        }

        vector<optional<DockNode>> nodes_;
        vector<u32> free_list_;
        // Index 0 is only the root's *initial* pool slot — a collapse (see collapse_empty_leaf())
        // can repoint this to a different slot when the root Split itself gets pruned away, so
        // root() always reads this rather than assuming index 0.
        DockNodeId root_ = static_cast<DockNodeId>(0);
    };

} // namespace SFT::UI::Docking
