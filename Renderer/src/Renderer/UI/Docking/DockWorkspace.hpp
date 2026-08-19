#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Renderer/UI/Button.hpp>
#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/DragGesture.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/Docking/DockLayout.hpp>
#include <Renderer/UI/Docking/DockTypes.hpp>

using std::optional;
using std::unordered_map;
using std::vector;


namespace SFT::UI::Docking {

    struct DockWorkspaceStyle {
        f32 tab_strip_height = 28.0f;
        f32 divider_thickness = 6.0f;
        f32 min_leaf_size = 80.0f;


        f32 drag_start_threshold = 4.0f;


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


    struct DockTearOffRequest {
        DockPanelId panel;
        glm::vec2 workspace_local_drop_position{0.0f};
    };

    struct DockWorkspaceEvents {
        vector<DockTearOffRequest> tear_off_requests;
        vector<DockPanelId> close_requests;
    };


    struct DockActiveTabDragSnapshot {
        DockPanelId panel;
        DockNodeId source_leaf{};
        glm::vec2 workspace_local_pointer_position{0.0f};
    };


    class DockWorkspace {
      public:


        /// Constructs a `DockWorkspace` from the supplied initialization values.
        ///
        /// @param id_prefix `id_prefix` value used by the operation.
        /// @param style `style` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit DockWorkspace(UString id_prefix, DockWorkspaceStyle style = {});

        /// Sets the content background for this `DockWorkspace`.
        ///
        /// @param color `color` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_content_background(Color color) noexcept;


        /// Adds panel using the supplied arguments and current state.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param placement `placement` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        bool add_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt);


        /// Performs the accept panel operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        /// @param placement `placement` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        bool accept_panel(DockPanelDesc desc, optional<DockPlacement> placement = std::nullopt);

        /// Removes the panel from its owning collection or system.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void remove_panel(const DockPanelId &id);


        /// Performs the take panel operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<DockPanelDesc> take_panel(const DockPanelId &id);

        /// Reports whether this `DockWorkspace` has panel.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_panel(const DockPanelId &id) const noexcept;

        /// Performs the panel desc operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const DockPanelDesc *panel_desc(const DockPanelId &id) const noexcept;


        /// Reports whether this `DockWorkspace` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;


        /// Returns the current or globally available focused leaf value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<DockNodeId> focused_leaf() const noexcept;

        /// Returns the current or globally available active tab drag value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<DockActiveTabDragSnapshot> active_tab_drag() const;


        /// Performs the begin frame operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param workspace_rect `workspace_rect` value used by the operation.
        /// @param delta_seconds `delta_seconds` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void begin_frame(Context &ctx, DockRect workspace_rect, f32 delta_seconds);


        /// Performs the panel content region operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ElementDecl> panel_content_region(const DockPanelId &id) const;


        /// Performs the end frame operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] DockWorkspaceEvents end_frame(Context &ctx);


        /// Performs the preview foreign drag operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param local_pointer `local_pointer` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<DockPlacement> preview_foreign_drag(Context &ctx,
                                                                    glm::vec2 local_pointer);


        /// Performs the preview foreign drag operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param foreign_panel `foreign_panel` value used by the operation.
        /// @param local_pointer `local_pointer` value used by the operation.
        /// @param released `released` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool preview_foreign_drag(Context &ctx, const DockPanelId &foreign_panel,
                                  glm::vec2 local_pointer, bool released);

        /// Clears foreign drag preview.
        ///
        /// @note This function does not throw exceptions.
        void clear_foreign_drag_preview() noexcept;

      private:


        struct ActiveDrag {
            enum class Kind : u8 { Tab, Divider } kind;
            DockPanelId panel;
            DockNodeId source_leaf{};
            DockNodeId resizing_node{};
            f32 anchor_ratio = 0.5f;
            glm::vec2 workspace_local_pointer_position{0.0f};
            optional<DockPlacement> hover_placement;
        };

        /// Returns the current or globally available workspace local rect value.
        ///
        /// @return Returns the current workspace local rect value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DockRect workspace_local_rect() const noexcept;

        /// Converts the value to context root representation.
        ///
        /// @param workspace_local `workspace_local` value used by the operation.
        ///
        /// @return Returns the value converted to context root representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 to_context_root(glm::vec2 workspace_local) const noexcept;

        /// Converts the value to workspace local representation.
        ///
        /// @param result `result` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DragGestureState::UpdateResult to_workspace_local(
            DragGestureState::UpdateResult result) const noexcept;

        /// Returns the current or globally available default target leaf value.
        ///
        /// @return Returns the current default target leaf value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DockNodeId default_target_leaf() const noexcept;

        /// Reports whether valid placement holds for this `DockWorkspace`.
        ///
        /// @param placement `placement` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid_placement(const DockPlacement &placement) const noexcept;

        /// Applies placement using the supplied arguments and current state.
        ///
        /// @param target `target` value used by the operation.
        /// @param zone `zone` value used by the operation.
        /// @param panel `panel` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool apply_placement(DockNodeId target, DockDropZone zone, const DockPanelId &panel);

        /// Resolves the layout associated with the supplied key, handle, or resource.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const DockNodeLayout *layout_for(DockNodeId id) const noexcept;

        /// Resolves the tab ID associated with the supplied key, handle, or resource.
        ///
        /// @param panel `panel` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString tab_id_for(const DockPanelId &panel) const;

        /// Resolves the close button ID associated with the supplied key, handle, or resource.
        ///
        /// @param panel `panel` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString close_button_id_for(const DockPanelId &panel) const;

        /// Resolves the divider ID associated with the supplied key, handle, or resource.
        ///
        /// @param node `node` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString divider_id_for(DockNodeId node) const;

        /// Performs the tab index under pointer operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param leaf `leaf` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<usize> tab_index_under_pointer(Context &ctx, DockNodeId leaf) const;

        /// Performs the classify drop zone operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param rect `rect` value used by the operation.
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static DockDropZone classify_drop_zone(const DockRect &rect, glm::vec2 point) noexcept;


        /// Performs the hit test drop target operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param point `point` value used by the operation.
        /// @param exclude `exclude` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<DockPlacement> hit_test_drop_target(glm::vec2 point, optional<DockNodeId> exclude) const;

        /// Performs the reorder within leaf operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param active `active` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void reorder_within_leaf(Context &ctx, const ActiveDrag &active);


        /// Updates tab drag hover from the supplied values.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param active `active` value used by the operation.
        /// @param r `r` value used by the operation.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        void update_tab_drag_hover(Context &ctx, ActiveDrag &active, const DragGestureState::UpdateResult &r);


        /// Resolves tab drag end into the concrete value used by the engine.
        ///
        /// @param active `active` value used by the operation.
        /// @param r `r` value used by the operation.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        void resolve_tab_drag_end(const ActiveDrag &active, const DragGestureState::UpdateResult &r);

        /// Applies divider delta using the supplied arguments and current state.
        ///
        /// @param active `active` value used by the operation.
        /// @param r `r` value used by the operation.
        ///
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        void apply_divider_delta(const ActiveDrag &active, const DragGestureState::UpdateResult &r);


        /// Updates drag state from the supplied values.
        ///
        /// @param ctx `ctx` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void update_drag_state(Context &ctx);

        /// Performs the continue active drag operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void continue_active_drag(Context &ctx);

        /// Performs the scan for new drag operation for `DockWorkspace` using the supplied arguments.
        ///
        /// @param ctx `ctx` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void scan_for_new_drag(Context &ctx);

        /// Draws leaf chrome using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param nl `nl` value used by the operation.
        /// @param n `n` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_leaf_chrome(Context &ctx, const DockNodeLayout &nl, const DockNode &n);

        /// Draws divider using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param nl `nl` value used by the operation.
        /// @param axis `axis` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_divider(Context &ctx, const DockNodeLayout &nl, DockSplitAxis axis);

        /// Draws chrome using the current rendering state.
        ///
        /// @param ctx `ctx` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_chrome(Context &ctx);

        /// Resolves the draw drop guide associated with the supplied key, handle, or resource.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param placement `placement` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
