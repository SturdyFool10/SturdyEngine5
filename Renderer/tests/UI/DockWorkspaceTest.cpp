#include <Renderer/UI/UI.hpp>

#include <cmath>
#include <iostream>

using SFT::UI::Docking::DockActiveTabDragSnapshot;
using SFT::UI::Docking::DockDropZone;
using SFT::UI::Docking::DockNode;
using SFT::UI::Docking::DockNodeId;
using SFT::UI::Docking::DockNodeLayout;
using SFT::UI::Docking::DockPanelDesc;
using SFT::UI::Docking::DockPlacement;
using SFT::UI::Docking::DockRect;
using SFT::UI::Docking::DockSplitAxis;
using SFT::UI::Docking::DockTree;
using SFT::UI::Docking::DockWorkspace;
using SFT::UI::Docking::DockWorkspaceEvents;
using SFT::UI::Docking::compute_dock_layout;
using SFT::UI::Docking::invalid_dock_node_id;
using SFT::Core::RendererExpected;
using SFT::UI::Context;
using SFT::UI::DragGestureState;
using SFT::UI::ElementDecl;
using SFT::UI::PointerState;

namespace {

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Performs the nearly operation using the supplied arguments.
    ///
    /// @param a `a` value used by the operation.
    /// @param b `b` value used by the operation.
    /// @param eps `eps` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool nearly(f32 a, f32 b, f32 eps = 0.5f) { return std::fabs(a - b) <= eps; }

    /// Returns the current or globally available drag gesture handles zero threshold and same frame release value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool drag_gesture_handles_zero_threshold_and_same_frame_release() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        const UString id{"gesture"};
        const ElementDecl decl{
            .sizing = {SFT::UI::SizingAxis::fixed(100.0f), SFT::UI::SizingAxis::fixed(20.0f)},
            .id = id,
        };

        const auto declare = [&]() {
            auto scope = ctx.element(decl);
            (void)scope;
        };

        ctx.begin_layout({200.0f, 100.0f});
        declare();
        (void)ctx.finish_frame();

        DragGestureState immediate;
        ctx.begin_layout({200.0f, 100.0f}, PointerState{
                                                    .position = {10.0f, 10.0f},
                                                    .down = true,
                                                    .pressed = true,
                                                });
        const DragGestureState::UpdateResult pressed = immediate.update(ctx, id, 0.0f);
        declare();
        (void)ctx.finish_frame();

        bool passed = check(pressed.started && pressed.active, "zero-threshold drag did not start on capture frame");
        passed &= check(!pressed.ended && immediate.is_capturing(), "held zero-threshold drag ended on press");

        ctx.begin_layout({200.0f, 100.0f}, PointerState{
                                                    .position = {40.0f, 10.0f},
                                                    .released = true,
                                                });
        const DragGestureState::UpdateResult released = immediate.update(ctx, id, 0.0f);
        declare();
        (void)ctx.finish_frame();
        passed &= check(released.active && released.ended && released.committed,
                        "zero-threshold release did not report final active committed frame");
        passed &= check(nearly(released.delta_since_start.x, 30.0f, 1.0e-4f),
                        "zero-threshold drag delta was not measured from its start");
        passed &= check(!immediate.is_capturing() && !ctx.pointer_captured(),
                        "zero-threshold release leaked pointer capture");

        DragGestureState click;
        ctx.begin_layout({200.0f, 100.0f}, PointerState{
                                                    .position = {10.0f, 10.0f},
                                                    .pressed = true,
                                                    .released = true,
                                                });
        const DragGestureState::UpdateResult same_frame = click.update(ctx, id, 4.0f);
        declare();
        (void)ctx.finish_frame();
        passed &= check(same_frame.ended && !same_frame.committed && !same_frame.cancelled,
                        "same-frame press/release was not resolved as an ordinary click");
        passed &= check(!click.is_capturing() && !ctx.pointer_captured(),
                        "same-frame press/release leaked pointer capture");

        DragGestureState immediate_same_frame;
        ctx.begin_layout({200.0f, 100.0f}, PointerState{
                                                    .position = {10.0f, 10.0f},
                                                    .pressed = true,
                                                    .released = true,
                                                });
        const DragGestureState::UpdateResult immediate_release =
            immediate_same_frame.update(ctx, id, 0.0f);
        declare();
        (void)ctx.finish_frame();
        passed &= check(immediate_release.started && immediate_release.active &&
                            immediate_release.ended && immediate_release.committed,
                        "same-frame zero-threshold gesture did not start and finish immediately");
        passed &= check(!immediate_same_frame.is_capturing() && !ctx.pointer_captured(),
                        "same-frame zero-threshold gesture leaked pointer capture");
        return passed;
    }


    /// Reports whether fresh tree is a single empty root leaf.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool fresh_tree_is_a_single_empty_root_leaf() {
        DockTree tree;
        bool passed = check(tree.empty(), "fresh tree is not reported empty");
        const DockNode *root = tree.node(tree.root());
        passed &= check(root != nullptr, "fresh tree has no root node");
        passed &= check(root->kind == DockNode::Kind::Leaf, "fresh tree root is not a leaf");
        passed &= check(root->tabs.empty(), "fresh tree root leaf is not empty");
        return passed;
    }

    /// Returns the current or globally available merge into leaf appends and activates value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool merge_into_leaf_appends_and_activates() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.merge_into_leaf(tree.root(), UString{"B"});
        const DockNode *root = tree.node(tree.root());
        bool passed = check(root->tabs.size() == 2, "leaf does not have two tabs after two merges");
        passed &= check(root->tabs[0] == UString{"A"} && root->tabs[1] == UString{"B"}, "tabs not in insertion order");
        passed &= check(root->active_tab_index == 1, "newly merged tab did not become active");
        return passed;
    }

    /// Splits leaf creates two children with expected ratio and tabs using the supplied arguments and current state.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool split_leaf_creates_two_children_with_expected_ratio_and_tabs() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.merge_into_leaf(tree.root(), UString{"B"});
        tree.split_leaf(tree.root(), DockSplitAxis::Horizontal,                 false, UString{"C"}, 0.25f);

        const DockNode *root = tree.node(tree.root());
        bool passed = check(root->kind == DockNode::Kind::Split, "split_leaf did not turn the target into a Split");
        passed &= check(root->split_axis == DockSplitAxis::Horizontal, "wrong split axis stored");
        passed &= check(nearly(root->split_ratio, 0.75f, 1.0e-4f), "wrong split ratio for panel_first=false");

        const DockNode *first = tree.node(root->first_child);
        const DockNode *second = tree.node(root->second_child);
        passed &= check(first != nullptr && second != nullptr, "split children missing");
        passed &= check(first->tabs.size() == 2 && first->tabs[0] == UString{"A"} && first->tabs[1] == UString{"B"},
                        "first child did not keep the original leaf's tabs");
        passed &= check(second->tabs.size() == 1 && second->tabs[0] == UString{"C"}, "second child does not hold only the new panel");
        return passed;
    }

    /// Removes the panel from multi tab leaf keeps the leaf from its owning collection or system.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool remove_panel_from_multi_tab_leaf_keeps_the_leaf() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.merge_into_leaf(tree.root(), UString{"B"});
        tree.remove_panel(UString{"A"});
        const DockNode *root = tree.node(tree.root());
        bool passed = check(root->kind == DockNode::Kind::Leaf, "leaf structure changed on a non-emptying removal");
        passed &= check(root->tabs.size() == 1 && root->tabs[0] == UString{"B"}, "wrong tab left after removal");
        return passed;
    }

    /// Removes the panel collapses two leaf split to the surviving sibling from its owning collection or system.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool remove_panel_collapses_two_leaf_split_to_the_surviving_sibling() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.split_leaf(tree.root(), DockSplitAxis::Horizontal, false, UString{"B"});

        tree.remove_panel(UString{"B"});
        const DockNode *root = tree.node(tree.root());
        bool passed = check(root != nullptr, "root vanished entirely after collapse");
        passed &= check(root->kind == DockNode::Kind::Leaf, "root did not collapse back to a leaf");
        passed &= check(root->tabs.size() == 1 && root->tabs[0] == UString{"A"}, "surviving leaf lost its tab");
        passed &= check(!tree.empty(), "tree incorrectly reports empty with a surviving panel");
        return passed;
    }

    /// Removes the panel promotes sibling into grandparent slot from its owning collection or system.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool remove_panel_promotes_sibling_into_grandparent_slot() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.split_leaf(tree.root(), DockSplitAxis::Horizontal, false, UString{"B"});
        const DockNode *root_before = tree.node(tree.root());
        const DockNodeId leaf_a = root_before->first_child;
        tree.split_leaf(leaf_a, DockSplitAxis::Vertical, false, UString{"C"});

        tree.remove_panel(UString{"C"});

        const DockNode *root = tree.node(tree.root());
        bool passed = check(root->kind == DockNode::Kind::Split, "outer split collapsed when it shouldn't have");
        const DockNode *first = tree.node(root->first_child);
        passed &= check(first != nullptr && first->kind == DockNode::Kind::Leaf, "inner split did not collapse to a leaf");
        passed &= check(first->tabs.size() == 1 && first->tabs[0] == UString{"A"}, "wrong panel promoted after inner collapse");
        const DockNode *second = tree.node(root->second_child);
        passed &= check(second != nullptr && second->tabs.size() == 1 && second->tabs[0] == UString{"B"}, "unrelated sibling B was disturbed");
        return passed;
    }

    /// Returns the current or globally available reorder and set active tab work value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool reorder_and_set_active_tab_work() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.merge_into_leaf(tree.root(), UString{"B"});
        tree.merge_into_leaf(tree.root(), UString{"C"});
        tree.reorder_tab(tree.root(), 0, 2);
        const DockNode *root = tree.node(tree.root());
        bool passed = check(root->tabs.size() == 3 && root->tabs[0] == UString{"B"} && root->tabs[1] == UString{"C"} &&
                                root->tabs[2] == UString{"A"},
                            "reorder_tab did not produce the expected order");
        tree.set_active_tab(tree.root(), UString{"C"});
        passed &= check(root->active_tab_index == 1, "set_active_tab did not select the requested panel");
        return passed;
    }


    /// Computes dock layout matches hand computed rects using the supplied arguments and current state.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool compute_dock_layout_matches_hand_computed_rects() {
        DockTree tree;
        tree.merge_into_leaf(tree.root(), UString{"A"});
        tree.split_leaf(tree.root(), DockSplitAxis::Horizontal, false, UString{"B"}, 0.25f);

        const vector<DockNodeLayout> layout = compute_dock_layout(tree, DockRect{{0, 0}, {800, 600}}, 28.0f, 6.0f);
        bool passed = check(layout.size() == 3, "expected exactly 3 layout entries (root split + 2 leaves)");

        const DockNode *root = tree.node(tree.root());
        const DockNodeLayout *split_layout = nullptr;
        const DockNodeLayout *first_layout = nullptr;
        const DockNodeLayout *second_layout = nullptr;
        for (const DockNodeLayout &nl : layout) {
            if (nl.node == tree.root()) split_layout = &nl;
            if (nl.node == root->first_child) first_layout = &nl;
            if (nl.node == root->second_child) second_layout = &nl;
        }
        passed &= check(split_layout && first_layout && second_layout, "could not find all three expected nodes in the layout output");
        if (!passed) {
            return passed;
        }

        passed &= check(nearly(split_layout->divider_rect.origin.x, 597.0f), "divider not at the expected x position");
        passed &= check(nearly(split_layout->divider_rect.size.x, 6.0f), "divider thickness not honored");
        passed &= check(nearly(first_layout->full_rect.size.x, 597.0f), "first child width does not match the 0.75 ratio");
        passed &= check(nearly(second_layout->full_rect.origin.x, 603.0f), "second child does not start right after the divider");
        passed &= check(nearly(first_layout->tab_strip_rect.size.y, 28.0f), "tab strip height not reserved");
        passed &= check(nearly(first_layout->content_rect.size.y, first_layout->full_rect.size.y - 28.0f),
                        "content rect does not account for the tab strip");
        return passed;
    }


    struct Frame {
        Context &ctx;
        DockWorkspace &ws;
        DockRect rect;

        /// Runs the requested work.
        ///
        /// @param pointer Pointer to the object or storage used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DockWorkspaceEvents run(const PointerState &pointer) {
            ctx.begin_layout(rect.origin + rect.size, pointer, 0.016f);
            ws.begin_frame(ctx, rect, 0.016f);
            DockWorkspaceEvents events = ws.end_frame(ctx);
            (void)ctx.finish_frame();
            return events;
        }
    };

    /// Returns the current or globally available duplicate and invalid panel placements are rejected atomically value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool duplicate_and_invalid_panel_placements_are_rejected_atomically() {
        DockWorkspace ws{UString{"atomic"}};
        bool passed = check(ws.add_panel(DockPanelDesc{
                                .id = UString{"A"},
                                .title = UString{"Original"},
                                .closable = false,
                            }),
                            "initial panel insertion failed");
        passed &= check(!ws.add_panel(DockPanelDesc{
                             .id = UString{"A"},
                             .title = UString{"Replacement"},
                         }),
                        "duplicate panel id was accepted");
        const DockPanelDesc *original = ws.panel_desc(UString{"A"});
        passed &= check(original != nullptr && original->title == UString{"Original"} && !original->closable,
                        "duplicate insertion changed the original descriptor");

        passed &= check(!ws.add_panel(DockPanelDesc{
                             .id = UString{"B"},
                             .title = UString{"Invalid target"},
                         },
                         DockPlacement{invalid_dock_node_id, DockDropZone::Center}),
                        "invalid explicit placement was accepted");
        passed &= check(!ws.has_panel(UString{"B"}) && ws.panel_desc(UString{"B"}) == nullptr,
                        "invalid placement left orphaned panel metadata");

        const optional<DockPanelDesc> taken = ws.take_panel(UString{"A"});
        passed &= check(taken.has_value() && taken->title == UString{"Original"},
                        "take_panel did not return the original descriptor");
        passed &= check(ws.empty() && !ws.has_panel(UString{"A"}),
                        "rejected duplicate left an extra tree entry behind");
        return passed;
    }

    /// Reports whether panel transfer preserves descriptor and foreign preview is non mutating.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool panel_transfer_preserves_descriptor_and_foreign_preview_is_non_mutating() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace source{UString{"source"}};
        DockWorkspace target{UString{"target"}};
        source.add_panel(DockPanelDesc{
            .id = UString{"Moved"},
            .title = UString{"Preserved title"},
            .closable = false,
        });
        target.add_panel(DockPanelDesc{.id = UString{"Target"}, .title = UString{"Target panel"}});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame target_frame{ctx, target, rect};
        target_frame.run(PointerState{.position = {-100, -100}});

        const optional<DockPlacement> placement = target.preview_foreign_drag(ctx, {400.0f, 314.0f});
        bool passed = check(placement.has_value() && placement->zone == DockDropZone::Center,
                            "foreign preview did not return the expected center placement");
        passed &= check(!target.has_panel(UString{"Moved"}),
                        "foreign preview mutated the target without a descriptor");
        const bool compatible_preview =
            target.preview_foreign_drag(ctx, UString{"Moved"}, {400.0f, 314.0f}, true);
        passed &= check(compatible_preview && !target.has_panel(UString{"Moved"}),
                        "compatibility foreign preview inserted an id-only panel");

        const optional<DockPanelDesc> moved = source.take_panel(UString{"Moved"});
        passed &= check(moved.has_value() && source.empty(),
                        "take_panel did not remove the panel from its source");
        if (!moved || !placement) {
            return false;
        }
        passed &= check(target.accept_panel(*moved, *placement),
                        "accept_panel rejected a valid transferred descriptor");
        const DockPanelDesc *accepted = target.panel_desc(UString{"Moved"});
        passed &= check(accepted != nullptr && accepted->title == UString{"Preserved title"} &&
                            !accepted->closable,
                        "transferred descriptor lost title or closability");
        return passed;
    }

    /// Returns the current or globally available dragging the divider resizes the split value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool dragging_the_divider_resizes_the_split() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"test"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});
        const DockNodeId root_leaf = *ws.focused_leaf();
        ws.add_panel(DockPanelDesc{.id = UString{"C"}, .title = UString{"Panel C"}}, DockPlacement{root_leaf, DockDropZone::Right});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame frame{ctx, ws, rect};

        frame.run(PointerState{.position = {0, 0}, .down = false});
        const optional<ElementDecl> before = ws.panel_content_region(UString{"C"});
        bool passed = check(before.has_value(), "C is not the active tab of its own leaf before resizing");
        passed &= check(nearly(before->floating.offset.x, 603.0f), "unexpected initial content position for C");

        frame.run(PointerState{.position = {600, 300}, .down = true});
        frame.run(PointerState{.position = {650, 300}, .down = true});
        frame.run(PointerState{.position = {650, 300}, .down = false});

        const optional<ElementDecl> after = ws.panel_content_region(UString{"C"});
        passed &= check(after.has_value(), "C stopped being the active tab after an unrelated resize");
        passed &= check(nearly(after->floating.offset.x, 653.0f), "divider drag did not move the split by the expected amount");
        return passed;
    }

    /// Returns the current or globally available clicking a tab makes it active value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool clicking_a_tab_makes_it_active() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"test"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame frame{ctx, ws, rect};

        frame.run(PointerState{.position = {-100, -100}, .down = false});
        bool passed = check(!ws.panel_content_region(UString{"A"}).has_value(), "A is unexpectedly active before any interaction");

        frame.run(PointerState{.position = {4, 14}, .down = true});
        frame.run(PointerState{.position = {4, 14}, .down = false});

        passed &= check(ws.panel_content_region(UString{"A"}).has_value(), "clicking A's tab did not make it active");
        passed &= check(!ws.panel_content_region(UString{"B"}).has_value(), "B is still reported active after A was clicked");
        return passed;
    }

    /// Returns the current or globally available clicking the close button removes only that panel value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool clicking_the_close_button_removes_only_that_panel() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"test"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame frame{ctx, ws, rect};

        frame.run(PointerState{.position = {-100, -100}, .down = false});


        DockWorkspaceEvents events = frame.run(PointerState{.position = {24, 14}, .down = true});

        bool passed = check(events.close_requests.size() == 1 && events.close_requests[0] == UString{"A"},
                            "close button press did not report exactly one close request for A");
        passed &= check(!ws.has_panel(UString{"A"}), "A was not actually removed after its close button was pressed");
        passed &= check(ws.has_panel(UString{"B"}), "unrelated panel B was removed too");
        passed &= check(ws.panel_content_region(UString{"B"}).has_value(), "B did not remain/become active after A closed");
        return passed;
    }

    /// Returns the current or globally available dragging a tab onto another leaf docks it there value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool dragging_a_tab_onto_another_leaf_docks_it_there() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"test"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});
        const DockNodeId leaf1 = *ws.focused_leaf();
        ws.add_panel(DockPanelDesc{.id = UString{"C"}, .title = UString{"Panel C"}}, DockPlacement{leaf1, DockDropZone::Right});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame frame{ctx, ws, rect};

        frame.run(PointerState{.position = {-100, -100}, .down = false});
        frame.run(PointerState{.position = {52, 14}, .down = true});
        frame.run(PointerState{.position = {701, 314}, .down = true});
        frame.run(PointerState{.position = {701, 314}, .down = false});

        bool passed = check(ws.panel_content_region(UString{"B"}).has_value(), "B did not become active in its new leaf");
        passed &= check(ws.panel_content_region(UString{"A"}).has_value(), "A did not become active in the leaf B vacated");
        passed &= check(!ws.panel_content_region(UString{"C"}).has_value(), "C is still reported active after B docked as a new tab");
        return passed;
    }

    /// Reports whether active tab drag snapshot is local and remove cancels it.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool active_tab_drag_snapshot_is_local_and_remove_cancels_it() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"active-drag"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});

        const DockRect rect{{100, 50}, {800, 600}};
        Frame frame{ctx, ws, rect};
        frame.run(PointerState{.position = {-100, -100}});
        frame.run(PointerState{.position = {152, 64}, .down = true});
        frame.run(PointerState{.position = {500, 250}, .down = true});

        const optional<DockActiveTabDragSnapshot> active = ws.active_tab_drag();
        bool passed = check(active.has_value() && active->panel == UString{"B"},
                            "active tab drag snapshot did not identify B");
        passed &= check(active.has_value() && nearly(active->workspace_local_pointer_position.x, 400.0f) &&
                            nearly(active->workspace_local_pointer_position.y, 200.0f),
                        "active tab drag snapshot was not workspace-local");

        ws.remove_panel(UString{"B"});
        passed &= check(!ws.active_tab_drag().has_value(),
                        "removing the actively dragged panel did not cancel workspace drag state");
        passed &= check(!ws.has_panel(UString{"B"}), "actively dragged panel was not removed");


        frame.run(PointerState{.position = {500, 250}, .down = false});
        ws.add_panel(DockPanelDesc{.id = UString{"C"}, .title = UString{"Panel C"}});
        frame.run(PointerState{.position = {-100, -100}, .down = false});
        frame.run(PointerState{.position = {152, 64}, .down = true});
        frame.run(PointerState{.position = {500, 250}, .down = true});
        const optional<DockActiveTabDragSnapshot> next = ws.active_tab_drag();
        passed &= check(next.has_value() && next->panel == UString{"C"},
                        "workspace could not begin a new drag after active-panel removal");
        frame.run(PointerState{.position = {500, 250}, .down = false});
        return passed;
    }

    /// Returns the current or globally available nonzero workspace origin translates rendering and tear off coordinates value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool nonzero_workspace_origin_translates_rendering_and_tear_off_coordinates() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"offset"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});

        const DockRect rect{{100, 50}, {800, 600}};
        Frame frame{ctx, ws, rect};
        frame.run(PointerState{.position = {-100, -100}});

        const optional<ElementDecl> content = ws.panel_content_region(UString{"B"});
        bool passed = check(content.has_value(), "offset workspace did not expose active content");
        passed &= check(content.has_value() && nearly(content->floating.offset.x, 100.0f) &&
                            nearly(content->floating.offset.y, 78.0f),
                        "content rect was not translated to Context-root coordinates");

        frame.run(PointerState{.position = {152, 64}, .down = true});
        frame.run(PointerState{.position = {950, 350}, .down = true});
        const DockWorkspaceEvents events =
            frame.run(PointerState{.position = {950, 350}, .down = false});
        passed &= check(events.tear_off_requests.size() == 1,
                        "offset workspace did not emit one tear-off request");
        if (!events.tear_off_requests.empty()) {
            const glm::vec2 local = events.tear_off_requests[0].workspace_local_drop_position;
            passed &= check(nearly(local.x, 850.0f) && nearly(local.y, 300.0f),
                            "tear-off position leaked Context-root workspace origin");
        }
        return passed;
    }

    /// Returns the current or globally available dragging a tab outside the workspace requests tear off value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool dragging_a_tab_outside_the_workspace_requests_tear_off() {
        RendererExpected<Context> made = Context::create(Context::Config{});
        if (!check(made.has_value(), "Context::create failed")) {
            return false;
        }
        Context ctx = std::move(*made);
        DockWorkspace ws{UString{"test"}};
        ws.add_panel(DockPanelDesc{.id = UString{"A"}, .title = UString{"Panel A"}});
        ws.add_panel(DockPanelDesc{.id = UString{"B"}, .title = UString{"Panel B"}});

        const DockRect rect{{0, 0}, {800, 600}};
        Frame frame{ctx, ws, rect};

        frame.run(PointerState{.position = {-100, -100}, .down = false});
        frame.run(PointerState{.position = {52, 14}, .down = true});
        frame.run(PointerState{.position = {900, 300}, .down = true});
        DockWorkspaceEvents events = frame.run(PointerState{.position = {900, 300}, .down = false});

        bool passed = check(events.tear_off_requests.size() == 1, "expected exactly one tear-off request");
        passed &= check(!events.tear_off_requests.empty() && events.tear_off_requests[0].panel == UString{"B"},
                        "tear-off request is for the wrong panel");
        passed &= check(ws.has_panel(UString{"B"}), "panel was removed from the workspace before its tear-off was confirmed");
        return passed;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = true;
    passed &= drag_gesture_handles_zero_threshold_and_same_frame_release();
    passed &= fresh_tree_is_a_single_empty_root_leaf();
    passed &= merge_into_leaf_appends_and_activates();
    passed &= split_leaf_creates_two_children_with_expected_ratio_and_tabs();
    passed &= remove_panel_from_multi_tab_leaf_keeps_the_leaf();
    passed &= remove_panel_collapses_two_leaf_split_to_the_surviving_sibling();
    passed &= remove_panel_promotes_sibling_into_grandparent_slot();
    passed &= reorder_and_set_active_tab_work();
    passed &= compute_dock_layout_matches_hand_computed_rects();
    passed &= duplicate_and_invalid_panel_placements_are_rejected_atomically();
    passed &= panel_transfer_preserves_descriptor_and_foreign_preview_is_non_mutating();
    passed &= dragging_the_divider_resizes_the_split();
    passed &= clicking_a_tab_makes_it_active();
    passed &= clicking_the_close_button_removes_only_that_panel();
    passed &= dragging_a_tab_onto_another_leaf_docks_it_there();
    passed &= active_tab_drag_snapshot_is_local_and_remove_cancels_it();
    passed &= nonzero_workspace_origin_translates_rendering_and_tear_off_coordinates();
    passed &= dragging_a_tab_outside_the_workspace_requests_tear_off();


    if (passed) {
        std::cout << "UIDockWorkspaceTest: all checks passed.\n";
    }
    return passed ? 0 : 1;
}
