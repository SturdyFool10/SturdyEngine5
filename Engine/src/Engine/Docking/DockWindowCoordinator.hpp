#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/WindowRequests.hpp>
#include <UI/Docking/Docking.hpp>

#include <functional>
#include <optional>
#include <unordered_map>

namespace SFT::Engine {

    // Host-facing coordination layer for one UI::Docking::DockWorkspace per managed OS window. It
    // never owns native windows or UI contexts: Application owns windows, callers own workspaces,
    // and WindowRequests is the deferred bridge between them.
    class DockWindowCoordinator {
      public:
        using DockWorkspace = UI::Docking::DockWorkspace;
        using DockPanelDesc = UI::Docking::DockPanelDesc;
        using DockPanelId = UI::Docking::DockPanelId;
        using DockPlacement = UI::Docking::DockPlacement;

        struct WorkspaceRegistration {
            DockWorkspace *workspace = nullptr;
            bool primary = false;
            bool close_when_empty = true;
        };

        // Called after a spawn completion. The owner creates/looks up the per-window UI context and
        // workspace, returning its address. Returning null declines the transfer; the origin keeps
        // the panel unchanged.
        using SpawnedWorkspaceResolver =
            std::function<DockWorkspace *(Core::RenderSurfaceHandle, const DockPanelDesc &)>;

        bool register_workspace(Platform::Windowing::WindowId window, DockWorkspace &workspace,
                                bool primary = false, bool close_when_empty = true) {
            return workspaces_.insert_or_assign(window, WorkspaceRegistration{&workspace, primary, close_when_empty})
                .second;
        }

        void unregister_workspace(Platform::Windowing::WindowId window) noexcept {
            workspaces_.erase(window);
            close_requests_.erase(window);
        }

        [[nodiscard]] DockWorkspace *workspace(Platform::Windowing::WindowId window) const noexcept {
            const auto found = workspaces_.find(window);
            return found != workspaces_.end() ? found->second.workspace : nullptr;
        }

        // Transactional existing-window transfer: target insertion happens first from a descriptor
        // copy; origin removal follows only after acceptance succeeds.
        bool transfer_panel(Platform::Windowing::WindowId origin_window,
                            Platform::Windowing::WindowId target_window,
                            const DockPanelId &panel,
                            std::optional<DockPlacement> placement = std::nullopt) {
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

        // Converts a release-outside event into a deferred spawn request. The panel remains in the
        // origin workspace until resolve_completion() observes a successful window spawn and target
        // workspace creation.
        [[nodiscard]] WindowRequestId request_tear_off(
            Platform::Windowing::WindowId origin_window,
            const UI::Docking::DockTearOffRequest &request,
            const Platform::Windowing::WindowConfig &window_config,
            WindowRequests &window_requests,
            Platform::Windowing::WindowFactory factory = nullptr) {
            DockWorkspace *origin = workspace(origin_window);
            if (origin == nullptr || origin->panel_desc(request.panel) == nullptr) {
                return {};
            }
            const WindowRequestId id = window_requests.spawn(window_config, factory);
            pending_spawns_.insert_or_assign(id.value, PendingSpawn{origin_window, request.panel});
            return id;
        }

        // Returns true when `completion` belonged to this coordinator. Spawn failure, target creation
        // failure, or target rejection all leave the origin unchanged.
        bool resolve_completion(const WindowRequestCompletion &completion,
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

        // Queue one close for each empty non-primary workspace. Repeated calls are idempotent until a
        // close completion unregisters the workspace.
        void request_empty_window_closes(WindowRequests &window_requests) {
            for (const auto &[window, registration] : workspaces_) {
                if (!registration.primary && registration.close_when_empty && registration.workspace != nullptr &&
                    registration.workspace->empty() && !close_requests_.contains(window)) {
                    close_requests_.insert_or_assign(window, window_requests.close(window));
                }
            }
        }

        [[nodiscard]] usize pending_spawn_count() const noexcept { return pending_spawns_.size(); }

      private:
        struct PendingSpawn {
            Platform::Windowing::WindowId origin_window{};
            DockPanelId panel;
        };

        std::unordered_map<Platform::Windowing::WindowId, WorkspaceRegistration> workspaces_;
        std::unordered_map<u64, PendingSpawn> pending_spawns_;
        std::unordered_map<Platform::Windowing::WindowId, WindowRequestId> close_requests_;
    };

} // namespace SFT::Engine
