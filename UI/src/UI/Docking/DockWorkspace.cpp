#include <UI/src/UI/Docking/DockWorkspace.hpp>


namespace SFT::UI::Docking {

    /// Sets the content background for this `Docking`.
    ///
    /// @param color `color` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DockWorkspace::set_content_background(Color color) noexcept { style_.content_background = color; }

    /// Adds panel using the supplied arguments and current state.
    ///
    /// @param desc Description of the resource or operation to perform.
    /// @param placement `placement` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    bool DockWorkspace::add_panel(DockPanelDesc desc, optional<DockPlacement> placement) {
        return accept_panel(std::move(desc), placement);
    }

    /// Performs the accept panel operation for `Docking` using the supplied arguments.
    ///
    /// @param desc Description of the resource or operation to perform.
    /// @param placement `placement` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    bool DockWorkspace::accept_panel(DockPanelDesc desc, optional<DockPlacement> placement) {
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

    /// Removes the panel from its owning collection or system.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::remove_panel(const DockPanelId &id) {
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

    /// Performs the take panel operation for `Docking` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<DockPanelDesc> DockWorkspace::take_panel(const DockPanelId &id) {
        const auto found = panels_.find(id);
        if (found == panels_.end()) {
            return std::nullopt;
        }
        DockPanelDesc desc = found->second;
        remove_panel(id);
        return desc;
    }

    /// Reports whether this `Docking` has panel.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool DockWorkspace::has_panel(const DockPanelId &id) const noexcept { return panels_.contains(id); }

    /// Performs the panel desc operation for `Docking` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const DockPanelDesc *DockWorkspace::panel_desc(const DockPanelId &id) const noexcept {
        const auto found = panels_.find(id);
        return found != panels_.end() ? &found->second : nullptr;
    }

    /// Reports whether this `Docking` contains no elements or payload.
    ///
    /// @return Returns the current empty value.
    /// @note This function does not throw exceptions.
    bool DockWorkspace::empty() const noexcept { return tree_.empty(); }

    /// Returns the current or globally available focused leaf value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    optional<DockNodeId> DockWorkspace::focused_leaf() const noexcept { return focused_leaf_; }

    /// Returns the current or globally available active tab drag value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<DockActiveTabDragSnapshot> DockWorkspace::active_tab_drag() const {
        if (!active_drag_ || active_drag_->kind != ActiveDrag::Kind::Tab) {
            return std::nullopt;
        }
        return DockActiveTabDragSnapshot{
            .panel = active_drag_->panel,
            .source_leaf = active_drag_->source_leaf,
            .workspace_local_pointer_position = active_drag_->workspace_local_pointer_position,
        };
    }

    /// Performs the begin frame operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param workspace_rect `workspace_rect` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::begin_frame(Context &ctx, DockRect workspace_rect, f32 delta_seconds) {
        last_delta_seconds_ = delta_seconds;
        workspace_rect_ = workspace_rect;
        last_layout_ = compute_dock_layout(tree_, workspace_local_rect(), style_.tab_strip_height,
                                           style_.divider_thickness);

        update_drag_state(ctx);


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

    /// Performs the panel content region operation for `Docking` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<ElementDecl> DockWorkspace::panel_content_region(const DockPanelId &id) const {
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


            .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                         .parent_attach_point = FloatingAttachPoint::LeftTop,
                         .offset = to_context_root(nl->content_rect.origin)},
            .debug_label = UString{"DockPanelContent"},
            .id = UString{id_prefix_.cpp_string() + "##content##" + id.cpp_string()},
        };
    }

    /// Performs the end frame operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DockWorkspaceEvents DockWorkspace::end_frame(Context &ctx) {
        (void)ctx;
        DockWorkspaceEvents events = std::move(pending_events_);
        pending_events_ = DockWorkspaceEvents{};
        for (const DockPanelId &closed : events.close_requests) {
            remove_panel(closed);
        }
        return events;
    }

    /// Performs the preview foreign drag operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param local_pointer `local_pointer` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<DockPlacement> DockWorkspace::preview_foreign_drag(Context &ctx,
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

    /// Performs the preview foreign drag operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param foreign_panel `foreign_panel` value used by the operation.
    /// @param local_pointer `local_pointer` value used by the operation.
    /// @param released `released` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool DockWorkspace::preview_foreign_drag(Context &ctx, const DockPanelId &foreign_panel,
                              glm::vec2 local_pointer, bool released) {
        (void)foreign_panel;
        (void)released;
        return preview_foreign_drag(ctx, local_pointer).has_value();
    }

    /// Clears foreign drag preview.
    ///
    /// @return Returns the current clear foreign drag preview value.
    /// @note This function does not throw exceptions.
    void DockWorkspace::clear_foreign_drag_preview() noexcept { foreign_drag_hover_.reset(); }

    /// Returns the current or globally available workspace local rect value.
    ///
    /// @return Returns the current workspace local rect value.
    /// @note This function does not throw exceptions.
    DockRect DockWorkspace::workspace_local_rect() const noexcept {
        return DockRect{.origin = glm::vec2{0.0f}, .size = workspace_rect_.size};
    }

    /// Converts the value to context root representation.
    ///
    /// @param workspace_local `workspace_local` value used by the operation.
    ///
    /// @return Returns the value converted to context root representation.
    /// @note This function does not throw exceptions.
    glm::vec2 DockWorkspace::to_context_root(glm::vec2 workspace_local) const noexcept {
        return workspace_rect_.origin + workspace_local;
    }

    /// Converts the value to workspace local representation.
    ///
    /// @param result `result` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    DragGestureState::UpdateResult DockWorkspace::to_workspace_local(
        DragGestureState::UpdateResult result) const noexcept {
        result.position -= workspace_rect_.origin;
        return result;
    }

    /// Returns the current or globally available default target leaf value.
    ///
    /// @return Returns the current default target leaf value.
    /// @note This function does not throw exceptions.
    DockNodeId DockWorkspace::default_target_leaf() const noexcept {
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

    /// Reports whether valid placement holds for this `Docking`.
    ///
    /// @param placement `placement` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool DockWorkspace::is_valid_placement(const DockPlacement &placement) const noexcept {
        const DockNode *target = tree_.node(placement.target_node);
        return target != nullptr && target->kind == DockNode::Kind::Leaf;
    }

    /// Applies placement using the supplied arguments and current state.
    ///
    /// @param target `target` value used by the operation.
    /// @param zone `zone` value used by the operation.
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool DockWorkspace::apply_placement(DockNodeId target, DockDropZone zone, const DockPanelId &panel) {
        bool applied = false;
        switch (zone) {
        case DockDropZone::Center: applied = tree_.merge_into_leaf(target, panel); break;
        case DockDropZone::Left:
            applied = tree_.split_leaf(target, DockSplitAxis::Horizontal,
                                                       true, panel);
            break;
        case DockDropZone::Right:
            applied = tree_.split_leaf(target, DockSplitAxis::Horizontal,
                                                       false, panel);
            break;
        case DockDropZone::Top:
            applied = tree_.split_leaf(target, DockSplitAxis::Vertical,
                                                       true, panel);
            break;
        case DockDropZone::Bottom:
            applied = tree_.split_leaf(target, DockSplitAxis::Vertical,
                                                       false, panel);
            break;
        }
        if (applied) {
            focused_leaf_ = tree_.find_leaf_of(panel);
        }
        return applied;
    }

    /// Resolves the layout associated with the supplied key, handle, or resource.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const DockNodeLayout *DockWorkspace::layout_for(DockNodeId id) const noexcept {
        for (const DockNodeLayout &nl : last_layout_) {
            if (nl.node == id) {
                return &nl;
            }
        }
        return nullptr;
    }

    /// Resolves the tab ID associated with the supplied key, handle, or resource.
    ///
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString DockWorkspace::tab_id_for(const DockPanelId &panel) const {
        return UString{id_prefix_.cpp_string() + "##tab##" + panel.cpp_string()};
    }

    /// Resolves the close button ID associated with the supplied key, handle, or resource.
    ///
    /// @param panel `panel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString DockWorkspace::close_button_id_for(const DockPanelId &panel) const {
        return UString{tab_id_for(panel).cpp_string() + "##close"};
    }

    /// Resolves the divider ID associated with the supplied key, handle, or resource.
    ///
    /// @param node `node` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    UString DockWorkspace::divider_id_for(DockNodeId node) const {
        return UString{id_prefix_.cpp_string() + "##divider##" + std::to_string(static_cast<u32>(node))};
    }

    /// Performs the tab index under pointer operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param leaf `leaf` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<usize> DockWorkspace::tab_index_under_pointer(Context &ctx, DockNodeId leaf) const {
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

    /// Performs the classify drop zone operation for `Docking` using the supplied arguments.
    ///
    /// @param rect `rect` value used by the operation.
    /// @param point `point` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    DockDropZone DockWorkspace::classify_drop_zone(const DockRect &rect, glm::vec2 point) noexcept {
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

    /// Performs the hit test drop target operation for `Docking` using the supplied arguments.
    ///
    /// @param point `point` value used by the operation.
    /// @param exclude `exclude` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<DockPlacement> DockWorkspace::hit_test_drop_target(glm::vec2 point, optional<DockNodeId> exclude) const {
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

    /// Performs the reorder within leaf operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param active `active` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::reorder_within_leaf(Context &ctx, const ActiveDrag &active) {
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

    /// Updates tab drag hover from the supplied values.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param active `active` value used by the operation.
    /// @param r `r` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    void DockWorkspace::update_tab_drag_hover(Context &ctx, ActiveDrag &active, const DragGestureState::UpdateResult &r) {
        const DockNodeLayout *source_nl = layout_for(active.source_leaf);
        if (source_nl != nullptr && source_nl->tab_strip_rect.contains(r.position)) {
            reorder_within_leaf(ctx, active);
            active.hover_placement.reset();
            return;
        }


        const DockNode *source_node = tree_.node(active.source_leaf);
        const bool source_is_splittable = source_node != nullptr && source_node->tabs.size() > 1;
        active.hover_placement =
            hit_test_drop_target(r.position, source_is_splittable ? std::nullopt : optional<DockNodeId>{active.source_leaf});
    }

    /// Resolves tab drag end into the concrete value used by the engine.
    ///
    /// @param active `active` value used by the operation.
    /// @param r `r` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    void DockWorkspace::resolve_tab_drag_end(const ActiveDrag &active, const DragGestureState::UpdateResult &r) {
        if (r.cancelled) {
            return;
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


    }

    /// Applies divider delta using the supplied arguments and current state.
    ///
    /// @param active `active` value used by the operation.
    /// @param r `r` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    void DockWorkspace::apply_divider_delta(const ActiveDrag &active, const DragGestureState::UpdateResult &r) {
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

    /// Updates drag state from the supplied values.
    ///
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::update_drag_state(Context &ctx) {
        if (active_drag_) {
            continue_active_drag(ctx);
            return;
        }
        scan_for_new_drag(ctx);
    }

    /// Performs the continue active drag operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::continue_active_drag(Context &ctx) {
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

    /// Performs the scan for new drag operation for `Docking` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::scan_for_new_drag(Context &ctx) {
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


                if (ctx.clicked(close_button_id_for(tab))) {
                    continue;
                }
                const DragGestureState::UpdateResult r = to_workspace_local(
                    tab_drag_[tab].update(ctx, tab_id_for(tab), style_.drag_start_threshold));
                if (r.ended && !r.committed && !r.cancelled) {

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

    /// Draws leaf chrome using the current rendering state.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param nl `nl` value used by the operation.
    /// @param n `n` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::draw_leaf_chrome(Context &ctx, const DockNodeLayout &nl, const DockNode &n) {
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
                    .cursor = active_drag_ && active_drag_->kind == ActiveDrag::Kind::Tab &&
                                      active_drag_->panel == panel_id
                                  ? style_.tab_dragging_cursor
                                  : style_.tab_cursor,
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
                        .cursor = style_.close_button_cursor,
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

    /// Draws divider using the current rendering state.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param nl `nl` value used by the operation.
    /// @param axis `axis` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::draw_divider(Context &ctx, const DockNodeLayout &nl, DockSplitAxis axis) {
        const CursorIcon resize_cursor = axis == DockSplitAxis::Horizontal ? style_.horizontal_divider_cursor
                                                                           : style_.vertical_divider_cursor;
        ButtonResult result = button(
            ctx,
            ElementDecl{
                .sizing = {SizingAxis::fixed(nl.divider_rect.size.x), SizingAxis::fixed(nl.divider_rect.size.y)},
                .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                             .parent_attach_point = FloatingAttachPoint::LeftTop,
                             .offset = to_context_root(nl.divider_rect.origin)},


                .cursor = resize_cursor,
                .debug_label = UString{"DockDivider"},
                .id = divider_id_for(nl.node),
            },
            style_.divider_style, divider_states_[nl.node], last_delta_seconds_);
        (void)result;


        if (divider_drag_[nl.node].is_capturing()) {
            ctx.force_cursor(resize_cursor);
        }
    }

    /// Draws chrome using the current rendering state.
    ///
    /// @param ctx `ctx` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::draw_chrome(Context &ctx) {
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

    /// Resolves the draw drop guide associated with the supplied key, handle, or resource.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param placement `placement` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWorkspace::draw_drop_guide_for(Context &ctx, const DockPlacement &placement) {
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


            .floating = {.attach_to = FloatingAttachTo::Root, .element_attach_point = FloatingAttachPoint::LeftTop,
                         .parent_attach_point = FloatingAttachPoint::LeftTop,
                         .offset = to_context_root(guide.origin), .z_index = 1000,
                         .capture_pointer = false},
            .debug_label = UString{"DockDropGuide"},
        });
        (void)scope;
    }

} // namespace SFT::UI::Docking


namespace SFT::UI::Docking {

    /// Performs the dock workspace operation for `Docking` using the supplied arguments.
    ///
    /// @param id_prefix `id_prefix` value used by the operation.
    /// @param style `style` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DockWorkspace::DockWorkspace(UString id_prefix, DockWorkspaceStyle style)
    : id_prefix_(std::move(id_prefix)), style_(style) {}

} // namespace SFT::UI::Docking

