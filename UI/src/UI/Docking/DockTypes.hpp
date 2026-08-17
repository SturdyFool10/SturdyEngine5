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


namespace SFT::UI::Docking {


    using DockPanelId = UString;


    enum class DockNodeId : u32 {};
    inline constexpr DockNodeId invalid_dock_node_id = static_cast<DockNodeId>(~u32{0});

    enum class DockSplitAxis : u8 {
        Horizontal,
        Vertical,
    };

    enum class DockDropZone : u8 { Center, Left, Right, Top, Bottom };


    struct DockRect {
        glm::vec2 origin{0.0f};
        glm::vec2 size{0.0f};

        /// Reports whether contains holds for this `DockRect`.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(glm::vec2 point) const noexcept;
    };

    struct DockNode {
        enum class Kind : u8 { Split, Leaf } kind = Kind::Leaf;


        DockSplitAxis split_axis = DockSplitAxis::Horizontal;
        f32 split_ratio = 0.5f;
        DockNodeId first_child = invalid_dock_node_id;
        DockNodeId second_child = invalid_dock_node_id;


        vector<DockPanelId> tabs;
        usize active_tab_index = 0;
    };

    struct DockPanelDesc {
        DockPanelId id;
        UString title;
        bool closable = true;
    };


    class DockTree {
      public:
        /// Constructs a `DockTree` in its default state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DockTree();

        /// Returns the current or globally available root value.
        ///
        /// @return Returns the current root value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DockNodeId root() const noexcept;

        /// Performs the node operation for `DockTree` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const DockNode *node(DockNodeId id) const noexcept;


        /// Performs the merge into leaf operation for `DockTree` using the supplied arguments.
        ///
        /// @param target `target` value used by the operation.
        /// @param panel `panel` value used by the operation.
        /// @param before `before` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        bool merge_into_leaf(DockNodeId target, DockPanelId panel,
                                           optional<usize> before = std::nullopt);


        /// Splits leaf using the supplied arguments and current state.
        ///
        /// @param target `target` value used by the operation.
        /// @param axis `axis` value used by the operation.
        /// @param panel_first `panel_first` value used by the operation.
        /// @param panel `panel` value used by the operation.
        /// @param new_panel_ratio `new_panel_ratio` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool split_leaf(DockNodeId target, DockSplitAxis axis, bool panel_first,
                                      DockPanelId panel, f32 new_panel_ratio = 0.25f);


        /// Removes the panel from its owning collection or system.
        ///
        /// @param panel `panel` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void remove_panel(const DockPanelId &panel);

        /// Finds leaf of in the available state.
        ///
        /// @param panel `panel` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<DockNodeId> find_leaf_of(const DockPanelId &panel) const noexcept;


        /// Reports whether this `DockTree` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;

        /// Sets the split ratio for this `DockTree`.
        ///
        /// @param id Identifier of the target object or resource.
        /// @param ratio `ratio` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_split_ratio(DockNodeId id, f32 ratio) noexcept;

        /// Sets the active tab for this `DockTree`.
        ///
        /// @param leaf `leaf` value used by the operation.
        /// @param panel `panel` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_active_tab(DockNodeId leaf, const DockPanelId &panel) noexcept;

        /// Performs the reorder tab operation for `DockTree` using the supplied arguments.
        ///
        /// @param leaf `leaf` value used by the operation.
        /// @param from_index Zero-based index of the target element or entry.
        /// @param to_index Zero-based index of the target element or entry.
        ///
        /// @note This function does not throw exceptions.
        void reorder_tab(DockNodeId leaf, usize from_index, usize to_index) noexcept;

      private:
        /// Performs the mutable node operation for `DockTree` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DockNode *mutable_node(DockNodeId id) noexcept;

        /// Performs the alloc node operation for `DockTree` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] DockNodeId alloc_node(DockNode value);

        /// Releases previously allocated storage or resources.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void free_node(DockNodeId id);

        /// Finds leaf of from in the available state.
        ///
        /// @param panel `panel` value used by the operation.
        /// @param from `from` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<DockNodeId> find_leaf_of_from(const DockPanelId &panel, DockNodeId from) const noexcept;

        struct ParentLink {
            DockNodeId parent{};
            bool is_first = false;
        };


        /// Finds parent in the available state.
        ///
        /// @param child `child` value used by the operation.
        /// @param from `from` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<ParentLink> find_parent(DockNodeId child, DockNodeId from) const noexcept;

        /// Performs the collapse empty leaf operation for `DockTree` using the supplied arguments.
        ///
        /// @param leaf_id Identifier of the target object or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void collapse_empty_leaf(DockNodeId leaf_id);

        vector<optional<DockNode>> nodes_;
        vector<u32> free_list_;


        DockNodeId root_ = static_cast<DockNodeId>(0);
    };

} // namespace SFT::UI::Docking
