#include <Engine/src/Engine/Docking/DockWindowCoordinator.hpp>


namespace SFT::Engine {

    bool DockWindowCoordinator::register_workspace(Platform::Windowing::WindowId window, DockWorkspace &workspace,
                            bool primary, bool close_when_empty) {
        return workspaces_.insert_or_assign(window, WorkspaceRegistration{&workspace, primary, close_when_empty})
            .second;
    }

    void DockWindowCoordinator::unregister_workspace(Platform::Windowing::WindowId window) noexcept {
        workspaces_.erase(window);
        close_requests_.erase(window);
    }

    DockWindowCoordinator::DockWorkspace *DockWindowCoordinator::workspace(Platform::Windowing::WindowId window) const noexcept {
        const auto found = workspaces_.find(window);
        return found != workspaces_.end() ? found->second.workspace : nullptr;
    }

    bool DockWindowCoordinator::transfer_panel(Platform::Windowing::WindowId origin_window,
                        Platform::Windowing::WindowId target_window,
                        const DockPanelId &panel,
                        std::optional<DockPlacement> placement) {
        DockWorkspace *origin = workspace(origin_window);
        DockWorkspace *target = workspace(target_window);
        if (origin == nullptr || target == nullptr || origin == target) {
            return false;
        }
        const DockPanelDesc *desc = origin->panel_desc(panel);
        if (desc == nullptr || !target->accept_panel(*desc, placement)) {
            return false;
        }
        origin->remove_panel(panel);
        return true;
    }

    WindowRequestId DockWindowCoordinator::request_tear_off(
        Platform::Windowing::WindowId origin_window,
        const UI::Docking::DockTearOffRequest &request,
        const Platform::Windowing::WindowConfig &window_config,
        WindowRequests &window_requests,
        Platform::Windowing::WindowFactory factory) {
        DockWorkspace *origin = workspace(origin_window);
        if (origin == nullptr || origin->panel_desc(request.panel) == nullptr) {
            return {};
        }
        const WindowRequestId id = window_requests.spawn(window_config, factory);
        pending_spawns_.insert_or_assign(id.value, PendingSpawn{origin_window, request.panel});
        return id;
    }

    bool DockWindowCoordinator::resolve_completion(const WindowRequestCompletion &completion,
                            const SpawnedWorkspaceResolver &resolve_spawned_workspace) {
        if (completion.kind == WindowRequestKind::Close) {
            if (close_requests_.erase(completion.window) > 0 && completion.accepted) {
                unregister_workspace(completion.window);
                return true;
            }
            return false;
        }

        const auto pending = pending_spawns_.find(completion.id.value);
        if (pending == pending_spawns_.end()) {
            return false;
        }
        const PendingSpawn request = pending->second;
        pending_spawns_.erase(pending);
        if (!completion.accepted || !completion.surface || !resolve_spawned_workspace) {
            return true;
        }

        DockWorkspace *origin = workspace(request.origin_window);
        if (origin == nullptr) {
            return true;
        }
        const DockPanelDesc *desc = origin->panel_desc(request.panel);
        if (desc == nullptr) {
            return true;
        }
        DockWorkspace *target = resolve_spawned_workspace(*completion.surface, *desc);
        if (target == nullptr || !target->accept_panel(*desc)) {
            return true;
        }

        register_workspace(completion.surface->window_id, *target, false, true);
        origin->remove_panel(request.panel);
        return true;
    }

    void DockWindowCoordinator::request_empty_window_closes(WindowRequests &window_requests) {
        for (const auto &[window, registration] : workspaces_) {
            if (!registration.primary && registration.close_when_empty && registration.workspace != nullptr &&
                registration.workspace->empty() && !close_requests_.contains(window)) {
                close_requests_.insert_or_assign(window, window_requests.close(window));
            }
        }
    }

    usize DockWindowCoordinator::pending_spawn_count() const noexcept { return pending_spawns_.size(); }

} // namespace SFT::Engine

