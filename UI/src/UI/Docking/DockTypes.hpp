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

/// Dock-tree data model: a node-pool tree of Split/Leaf nodes plus the mutating operations a
/// docking workspace needs (merge a panel into a tab group, split a leaf, remove a panel and
/// collapse the tree around the gap). Pure data + tree algebra — no Context/Clay/rendering
/// dependency at all, the same "usable standalone" split Style.hpp keeps from Context.hpp. See
/// DockWorkspace.hpp for the Context-facing, per-frame-drawn/dragged layer built on top of this.
namespace SFT::UI::Docking {

    /// Stable, app-assigned logical panel identity — a UString (not an opaque numeric handle) so it
    /// round-trips directly into DockLayoutDescriptor (Phase 5) and matches ElementDecl::id's own
    /// string-keyed stable-identity convention (Style.hpp).
    using DockPanelId = UString;

    /// Handle into DockTree's own node pool. Never persisted — DockLayoutDescriptor (Phase 5)
    /// serializes the tree's *shape*, not pool indices, since a restored tree gets a fresh pool.
    enum class DockNodeId : u32 {};
    inline constexpr DockNodeId invalid_dock_node_id = static_cast<DockNodeId>(~u32{0});

    enum class DockSplitAxis : u8 {
        Horizontal,
        Vertical,
    };

    enum class DockDropZone : u8 { Center, Left, Right, Top, Bottom };

    /// Plain float rect in an explicitly chosen pixel coordinate space. DockLayout preserves the
    /// coordinate space supplied by its caller; DockWorkspace solves layouts in workspace-local
    /// space and translates only when emitting Context-root floating elements. Kept float, not
    /// RHI::Rect2D's int, since split-ratio math wants ElementDecl::Sizing's precision.
    struct DockRect {
        glm::vec2 origin{0.0f};
        glm::vec2 size{0.0f};

        [[nodiscard]] bool contains(glm::vec2 point) const noexcept;
    };

    struct DockNode {
        enum class Kind : u8 { Split, Leaf } kind = Kind::Leaf;

        /// Kind::Split:
        DockSplitAxis split_axis = DockSplitAxis::Horizontal;
        f32 split_ratio = 0.5f;
        DockNodeId first_child = invalid_dock_node_id;
        DockNodeId second_child = invalid_dock_node_id;

        /// Kind::Leaf: an ordered tab strip. Empty only transiently — DockTree::remove_panel prunes
        /// an emptied non-root leaf by collapsing its parent split away (see that method's own
        /// comment); an empty *root* leaf is how DockTree::empty() reports "nothing left to show."
        vector<DockPanelId> tabs;
        usize active_tab_index = 0;
    };

    struct DockPanelDesc {
        DockPanelId id;
        UString title;
        bool closable = true;
    };

    /// Node-pool tree: one root plus however many Split/Leaf descendants. One DockTree per
    /// DockWorkspace (== per OS window once Phase 3 wires up tear-off — see that phase's own design
    /// note on why the two are 1:1).
    class DockTree {
      public:
        DockTree();

        [[nodiscard]] DockNodeId root() const noexcept;

        [[nodiscard]] const DockNode *node(DockNodeId id) const noexcept;

        /// Docks `panel` as a new tab in leaf `target`, at the end of its strip (or a specific index
        /// if `before` is set) and makes it the active tab. `panel` must not currently be anywhere
        /// else in this tree — callers that might be moving an already-placed panel must
        /// remove_panel() it first (see DockWorkspace's own drop-resolution sequencing for why that
        /// order is also what keeps `target`'s DockNodeId itself from being invalidated by the
        /// removal, when target is a *different* leaf than the one being vacated).
        bool merge_into_leaf(DockNodeId target, DockPanelId panel,
                                           optional<usize> before = std::nullopt);

        /// Splits leaf `target` along `axis`: `target` itself becomes a Split node with two new leaf
        /// children — one keeping `target`'s existing tabs, the other holding only `panel`.
        /// `panel_first` picks which child gets the new panel (true = first_child, i.e. left/top of
        /// the split — see DockSplitAxis's own doc comment for what "first" means per axis).
        bool split_leaf(DockNodeId target, DockSplitAxis axis, bool panel_first,
                                      DockPanelId panel, f32 new_panel_ratio = 0.25f);

        /// Removes `panel` from whichever leaf holds it. If that leaf becomes empty and isn't the
        /// root, the leaf and its now-redundant parent Split are both freed and the parent's slot
        /// (wherever it was referenced from) is replaced by the *sibling* — standard dock-tree
        /// pruning, mirroring ImGui's own DockNode collapse behavior. No-op if `panel` isn't in the
        /// tree at all (safe to call speculatively).
        void remove_panel(const DockPanelId &panel);

        [[nodiscard]] optional<DockNodeId> find_leaf_of(const DockPanelId &panel) const noexcept;

        /// True only when the root itself is an empty leaf — the tree has no panels left at all
        /// (DockWorkspace::empty() surfaces this to decide whether a torn-off window should close).
        [[nodiscard]] bool empty() const noexcept;

        void set_split_ratio(DockNodeId id, f32 ratio) noexcept;

        void set_active_tab(DockNodeId leaf, const DockPanelId &panel) noexcept;

        void reorder_tab(DockNodeId leaf, usize from_index, usize to_index) noexcept;

      private:
        [[nodiscard]] DockNode *mutable_node(DockNodeId id) noexcept;

        [[nodiscard]] DockNodeId alloc_node(DockNode value);

        void free_node(DockNodeId id);

        [[nodiscard]] optional<DockNodeId> find_leaf_of_from(const DockPanelId &panel, DockNodeId from) const noexcept;

        struct ParentLink {
            DockNodeId parent{};
            bool is_first = false;
        };

        /// Searches downward from `from` for the Split node whose first_child/second_child ==
        /// `child` — nodes don't store a parent pointer of their own (kept a strict tree, no back-
        /// edges), so collapse_empty_leaf() looks it up on the rare occasion (a panel closing/tearing
        /// off) that it actually needs to relink one.
        [[nodiscard]] optional<ParentLink> find_parent(DockNodeId child, DockNodeId from) const noexcept;

        void collapse_empty_leaf(DockNodeId leaf_id);

        vector<optional<DockNode>> nodes_;
        vector<u32> free_list_;
        /// Index 0 is only the root's *initial* pool slot — a collapse (see collapse_empty_leaf())
        /// can repoint this to a different slot when the root Split itself gets pruned away, so
        /// root() always reads this rather than assuming index 0.
        DockNodeId root_ = static_cast<DockNodeId>(0);
    };

} // namespace SFT::UI::Docking
