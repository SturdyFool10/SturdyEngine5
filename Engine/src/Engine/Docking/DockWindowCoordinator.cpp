#include <Engine/Docking/DockWindowCoordinator.hpp>


namespace SFT::Engine {

    /// Registers workspace using the supplied arguments and current state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param workspace `workspace` value used by the operation.
    /// @param primary `primary` value used by the operation.
    /// @param close_when_empty `close_when_empty` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool DockWindowCoordinator::register_workspace(WindowManager::WindowId window, DockWorkspace &workspace,
                            bool primary, bool close_when_empty) {
        return workspaces_.insert_or_assign(window, WorkspaceRegistration{&workspace, primary, close_when_empty})
            .second;
    }

    /// Unregisters workspace using the supplied arguments and current state.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DockWindowCoordinator::unregister_workspace(WindowManager::WindowId window) noexcept {
        workspaces_.erase(window);
        close_requests_.erase(window);
    }

    /// Performs the workspace operation for `Engine` using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    DockWindowCoordinator::DockWorkspace *DockWindowCoordinator::workspace(WindowManager::WindowId window) const noexcept {
        const auto found = workspaces_.find(window);
        return found != workspaces_.end() ? found->second.workspace : nullptr;
    }

    /// Performs the transfer panel operation for `Engine` using the supplied arguments.
    ///
    /// @param origin_window Window used or affected by the operation.
    /// @param target_window Window used or affected by the operation.
    /// @param panel `panel` value used by the operation.
    /// @param placement `placement` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    bool DockWindowCoordinator::transfer_panel(WindowManager::WindowId origin_window,
                        WindowManager::WindowId target_window,
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

    /// Requests tear off using the supplied arguments and current state.
    ///
    /// @param origin_window Window used or affected by the operation.
    /// @param request `request` value used by the operation.
    /// @param window_config Configuration values controlling the operation.
    /// @param window_requests Window used or affected by the operation.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    WindowRequestId DockWindowCoordinator::request_tear_off(
        WindowManager::WindowId origin_window,
        const UI::Docking::DockTearOffRequest &request,
        const WindowManager::WindowConfig &window_config,
        WindowRequests &window_requests,
        WindowManager::WindowFactory factory) {
        DockWorkspace *origin = workspace(origin_window);
        if (origin == nullptr || origin->panel_desc(request.panel) == nullptr) {
            return {};
        }
        const WindowRequestId id = window_requests.spawn(window_config, factory);
        pending_spawns_.insert_or_assign(id.value, PendingSpawn{origin_window, request.panel});
        return id;
    }

    /// Resolves completion into the concrete value used by the engine.
    ///
    /// @param completion `completion` value used by the operation.
    /// @param resolve_spawned_workspace `resolve_spawned_workspace` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Requests empty window closes using the supplied arguments and current state.
    ///
    /// @param window_requests Window used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void DockWindowCoordinator::request_empty_window_closes(WindowRequests &window_requests) {
        for (const auto &[window, registration] : workspaces_) {
            if (!registration.primary && registration.close_when_empty && registration.workspace != nullptr &&
                registration.workspace->empty() && !close_requests_.contains(window)) {
                close_requests_.insert_or_assign(window, window_requests.close(window));
            }
        }
    }

    /// Returns the pending spawn count for this `Engine`.
    ///
    /// @return Returns the current pending spawn count value.
    /// @note This function does not throw exceptions.
    usize DockWindowCoordinator::pending_spawn_count() const noexcept { return pending_spawns_.size(); }

} // namespace SFT::Engine

