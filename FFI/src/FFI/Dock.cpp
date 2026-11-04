/// C ABI for `SFT::UI::Docking::DockWorkspace`: a tabbed, splittable, drag-to-rearrange panel layout.
///
/// A workspace persists across frames and is driven inside an open UI frame (a `sturdy_ui_begin` frame or a UI tree builder); the open frame's layout
/// context comes from `foreign_ui_context()`, and panel content regions open on the same element stack the other `sturdy_ui_*` calls use. The events of
/// the last finished frame are kept in the record so their strings can be read back by index.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include <Engine/ForeignUi.hpp>
#include <Renderer/UI/Docking/Docking.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace Dock = SFT::UI::Docking;

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    struct DockRecord {
        explicit DockRecord(std::string_view prefix) : workspace(SFT::UString{prefix}) {}
        Dock::DockWorkspace workspace;
        Dock::DockWorkspaceEvents last_events;
    };

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    [[nodiscard]] SFT::UString panel_id(const char *id) { return SFT::UString::from_c_str(id); }

    template <class Body>
    [[nodiscard]] SturdyResult with_dock(SturdyDockWorkspace handle, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            void *pointer = nullptr;
            const SturdyResult resolved = resolve_handle(handle.token, HandleKind::DockWorkspace, &pointer);
            if (resolved != STURDY_OK) {
                return resolved;
            }
            return body(*static_cast<DockRecord *>(pointer));
        });
    }

    [[nodiscard]] SturdyResult to_native(const SturdyDockPlacement *placement, std::optional<Dock::DockPlacement> *out) noexcept {
        *out = std::nullopt;
        if (placement == nullptr || placement->has_placement == 0) {
            return STURDY_OK;
        }
        Dock::DockDropZone zone{};
        switch (placement->zone) {
        case STURDY_DOCK_ZONE_CENTER:
            zone = Dock::DockDropZone::Center;
            break;
        case STURDY_DOCK_ZONE_LEFT:
            zone = Dock::DockDropZone::Left;
            break;
        case STURDY_DOCK_ZONE_RIGHT:
            zone = Dock::DockDropZone::Right;
            break;
        case STURDY_DOCK_ZONE_TOP:
            zone = Dock::DockDropZone::Top;
            break;
        case STURDY_DOCK_ZONE_BOTTOM:
            zone = Dock::DockDropZone::Bottom;
            break;
        case STURDY_DOCK_ZONE_FORCE_U32:
        default:
            return invalid("unrecognized dock zone");
        }
        *out = Dock::DockPlacement{static_cast<Dock::DockNodeId>(placement->target_node), zone};
        return STURDY_OK;
    }

    [[nodiscard]] SturdyDockPlacement to_foreign(const Dock::DockPlacement &placement) noexcept {
        SturdyDockPlacement out{};
        out.has_placement = STURDY_TRUE;
        out.target_node = static_cast<uint32_t>(placement.target_node);
        switch (placement.zone) {
        case Dock::DockDropZone::Center:
            out.zone = STURDY_DOCK_ZONE_CENTER;
            break;
        case Dock::DockDropZone::Left:
            out.zone = STURDY_DOCK_ZONE_LEFT;
            break;
        case Dock::DockDropZone::Right:
            out.zone = STURDY_DOCK_ZONE_RIGHT;
            break;
        case Dock::DockDropZone::Top:
            out.zone = STURDY_DOCK_ZONE_TOP;
            break;
        case Dock::DockDropZone::Bottom:
            out.zone = STURDY_DOCK_ZONE_BOTTOM;
            break;
        }
        return out;
    }

    [[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

    [[nodiscard]] SturdyResult need_frame() noexcept {
        return set_error(STURDY_ERROR_BUSY, "no UI frame is open on this thread; call this between sturdy_ui_begin and sturdy_ui_end (or inside a UI tree builder)");
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_dock_create(const char *id_prefix, SturdyDockWorkspace *out_workspace) {
    return guarded([&]() -> SturdyResult {
        if (out_workspace == nullptr) {
            return invalid("output pointer must not be null");
        }
        auto record = std::make_unique<DockRecord>(id_prefix != nullptr ? std::string_view{id_prefix} : std::string_view{});
        out_workspace->token = mint_handle(HandleKind::DockWorkspace, record.get());
        static_cast<void>(record.release());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_destroy(SturdyDockWorkspace workspace) {
    return guarded([&]() -> SturdyResult {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(workspace.token, HandleKind::DockWorkspace, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(workspace.token);
        delete static_cast<DockRecord *>(pointer);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_add_panel(SturdyDockWorkspace workspace, const char *id, const char *title, SturdyBool closable,
                                                   const SturdyDockPlacement *placement) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr || title == nullptr) {
            return invalid("id and title must not be null");
        }
        std::optional<Dock::DockPlacement> native;
        if (const SturdyResult translated = to_native(placement, &native); translated != STURDY_OK) {
            return translated;
        }
        if (record.workspace.has_panel(panel_id(id))) {
            return set_error(STURDY_ERROR_COMPONENT_PRESENT, "a panel with that id is already in the workspace");
        }
        Dock::DockPanelDesc desc{};
        desc.id = panel_id(id);
        desc.title = SFT::UString::from_c_str(title);
        desc.closable = closable != 0;
        if (!record.workspace.add_panel(std::move(desc), native)) {
            return invalid("the placement is not valid in this workspace");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_remove_panel(SturdyDockWorkspace workspace, const char *id) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr) {
            return invalid("id must not be null");
        }
        record.workspace.remove_panel(panel_id(id));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_transfer_panel(SturdyDockWorkspace origin, SturdyDockWorkspace target, const char *id, const SturdyDockPlacement *placement) {
    return guarded([&]() -> SturdyResult {
        void *origin_pointer = nullptr;
        void *target_pointer = nullptr;
        if (const SturdyResult r = resolve_handle(origin.token, HandleKind::DockWorkspace, &origin_pointer); r != STURDY_OK) {
            return r;
        }
        if (const SturdyResult r = resolve_handle(target.token, HandleKind::DockWorkspace, &target_pointer); r != STURDY_OK) {
            return r;
        }
        if (id == nullptr) {
            return invalid("id must not be null");
        }
        if (origin_pointer == target_pointer) {
            return invalid("origin and target are the same workspace");
        }
        std::optional<Dock::DockPlacement> native;
        if (const SturdyResult translated = to_native(placement, &native); translated != STURDY_OK) {
            return translated;
        }
        auto &from = static_cast<DockRecord *>(origin_pointer)->workspace;
        auto &to = static_cast<DockRecord *>(target_pointer)->workspace;
        const SFT::UString panel = panel_id(id);
        const Dock::DockPanelDesc *desc = from.panel_desc(panel);
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the origin workspace has no such panel");
        }
        if (!to.accept_panel(*desc, native)) {
            return invalid("the target workspace rejected the panel (duplicate id or invalid placement)");
        }
        from.remove_panel(panel);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_has_panel(SturdyDockWorkspace workspace, const char *id, SturdyBool *out_has) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr || out_has == nullptr) {
            return invalid("id and output pointer must not be null");
        }
        *out_has = record.workspace.has_panel(panel_id(id)) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_is_empty(SturdyDockWorkspace workspace, SturdyBool *out_empty) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (out_empty == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_empty = record.workspace.empty() ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_panel_info(SturdyDockWorkspace workspace, const char *id, SturdyDockPanelInfo *out_info) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr || out_info == nullptr) {
            return invalid("id and output pointer must not be null");
        }
        const Dock::DockPanelDesc *desc = record.workspace.panel_desc(panel_id(id));
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such panel");
        }
        *out_info = {desc->closable ? STURDY_TRUE : STURDY_FALSE, static_cast<uint32_t>(desc->title.cpp_string_view().size())};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_panel_title(SturdyDockWorkspace workspace, const char *id, char *buffer, size_t capacity, size_t *out_length) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr) {
            return invalid("id must not be null");
        }
        const Dock::DockPanelDesc *desc = record.workspace.panel_desc(panel_id(id));
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such panel");
        }
        return copy_string_out(desc->title.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_focused_leaf(SturdyDockWorkspace workspace, uint32_t *out_node) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (out_node == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto leaf = record.workspace.focused_leaf();
        if (!leaf) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no leaf has focus");
        }
        *out_node = static_cast<uint32_t>(*leaf);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_active_tab_drag(SturdyDockWorkspace workspace, SturdyDockTabDrag *out_drag) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (out_drag == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto drag = record.workspace.active_tab_drag();
        if (!drag) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no tab is being dragged");
        }
        *out_drag = {static_cast<uint32_t>(drag->source_leaf), drag->workspace_local_pointer_position.x, drag->workspace_local_pointer_position.y,
                     static_cast<uint32_t>(drag->panel.cpp_string_view().size())};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_active_tab_drag_panel(SturdyDockWorkspace workspace, char *buffer, size_t capacity, size_t *out_length) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        const auto drag = record.workspace.active_tab_drag();
        if (!drag) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no tab is being dragged");
        }
        return copy_string_out(drag->panel.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_set_content_background(SturdyDockWorkspace workspace, const float rgba[4]) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (rgba == nullptr) {
            return invalid("color must not be null");
        }
        record.workspace.set_content_background(
            SFT::UI::Color{static_cast<SFT::f64>(rgba[0]), static_cast<SFT::f64>(rgba[1]), static_cast<SFT::f64>(rgba[2]), static_cast<SFT::f64>(rgba[3])});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_save_layout(SturdyDockWorkspace workspace, char *buffer, size_t capacity, size_t *out_length) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        return copy_string_out(Dock::serialize_dock_layout(record.workspace.save_layout()), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_restore_layout(SturdyDockWorkspace workspace, const char *layout) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (layout == nullptr) {
            return invalid("layout must not be null");
        }
        const auto parsed = Dock::parse_dock_layout(std::string_view{layout});
        if (!parsed) {
            return invalid("the layout text is malformed");
        }
        if (!record.workspace.restore_layout(*parsed)) {
            return invalid("the layout is not valid for this workspace (it names panels the workspace does not have)");
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_begin_frame(SturdyDockWorkspace workspace, float x, float y, float width, float height, float delta_seconds) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        SFT::UI::Context *context = SFT::Engine::foreign_ui_context();
        if (context == nullptr) {
            return need_frame();
        }
        if (!finite(x) || !finite(y) || !finite(width) || !finite(height) || !finite(delta_seconds) || width < 0.0f || height < 0.0f) {
            return invalid("the rectangle and delta must be finite, and the size not negative");
        }
        record.workspace.begin_frame(*context, Dock::DockRect{glm::vec2{x, y}, glm::vec2{width, height}}, delta_seconds);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_begin_panel_content(SturdyDockWorkspace workspace, const char *id) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (id == nullptr) {
            return invalid("id must not be null");
        }
        if (SFT::Engine::foreign_ui_context() == nullptr) {
            return need_frame();
        }
        const std::optional<SFT::UI::ElementDecl> region = record.workspace.panel_content_region(panel_id(id));
        if (!region) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the panel is hidden (not the active tab) or unknown");
        }
        auto opened = SFT::Engine::begin_foreign_ui_declared_element(*region);
        if (!opened) {
            return set_error(STURDY_ERROR_BUSY, opened.error().message);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_end_frame(SturdyDockWorkspace workspace, SturdyDockEvents *out_events) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        SFT::UI::Context *context = SFT::Engine::foreign_ui_context();
        if (context == nullptr) {
            return need_frame();
        }
        record.last_events = record.workspace.end_frame(*context);
        if (out_events != nullptr) {
            *out_events = {static_cast<uint32_t>(record.last_events.tear_off_requests.size()), static_cast<uint32_t>(record.last_events.close_requests.size())};
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_tear_off(SturdyDockWorkspace workspace, uint32_t index, float *out_x, float *out_y, char *buffer, size_t capacity,
                                                  size_t *out_length) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (index >= record.last_events.tear_off_requests.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "tear-off index is out of range");
        }
        const Dock::DockTearOffRequest &request = record.last_events.tear_off_requests[index];
        if (out_x != nullptr) {
            *out_x = request.workspace_local_drop_position.x;
        }
        if (out_y != nullptr) {
            *out_y = request.workspace_local_drop_position.y;
        }
        return copy_string_out(request.panel.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_close_request(SturdyDockWorkspace workspace, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        if (index >= record.last_events.close_requests.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "close request index is out of range");
        }
        return copy_string_out(record.last_events.close_requests[index].cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_preview_foreign_drag_target(SturdyDockWorkspace workspace, float local_x, float local_y,
                                                                    SturdyDockPlacement *out_placement) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        SFT::UI::Context *context = SFT::Engine::foreign_ui_context();
        if (context == nullptr) {
            return need_frame();
        }
        if (out_placement == nullptr || !finite(local_x) || !finite(local_y)) {
            return invalid("output pointer must not be null and the pointer position must be finite");
        }
        const auto placement = record.workspace.preview_foreign_drag(*context, glm::vec2{local_x, local_y});
        if (!placement) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the pointer targets no drop zone");
        }
        *out_placement = to_foreign(*placement);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_preview_foreign_drag(SturdyDockWorkspace workspace, const char *foreign_panel, float local_x, float local_y,
                                                              SturdyBool released, SturdyBool *out_applied) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        SFT::UI::Context *context = SFT::Engine::foreign_ui_context();
        if (context == nullptr) {
            return need_frame();
        }
        if (foreign_panel == nullptr || !finite(local_x) || !finite(local_y)) {
            return invalid("panel must not be null and the pointer position must be finite");
        }
        const bool applied = record.workspace.preview_foreign_drag(*context, panel_id(foreign_panel), glm::vec2{local_x, local_y}, released != 0);
        if (out_applied != nullptr) {
            *out_applied = applied ? STURDY_TRUE : STURDY_FALSE;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_dock_clear_foreign_drag_preview(SturdyDockWorkspace workspace) {
    return with_dock(workspace, [&](DockRecord &record) -> SturdyResult {
        record.workspace.clear_foreign_drag_preview();
        return STURDY_OK;
    });
}

} // extern "C"
