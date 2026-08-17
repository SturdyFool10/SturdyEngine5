#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/WindowRequests.hpp>
#include <UI/Docking/Docking.hpp>

#include <functional>
#include <optional>
#include <unordered_map>

namespace SFT::Engine {

    /// Host-facing coordination layer for one UI::Docking::DockWorkspace per managed OS window. It
    /// never owns native windows or UI contexts: Application owns windows, callers own workspaces,
    /// and WindowRequests is the deferred bridge between them.
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

        /// Called after a spawn completion. The owner creates/looks up the per-window UI context and
        /// workspace, returning its address. Returning null declines the transfer; the origin keeps
        /// the panel unchanged.
        using SpawnedWorkspaceResolver =
            std::function<DockWorkspace *(Core::RenderSurfaceHandle, const DockPanelDesc &)>;

        bool register_workspace(Platform::Windowing::WindowId window, DockWorkspace &workspace,
                                bool primary = false, bool close_when_empty = true);

        void unregister_workspace(Platform::Windowing::WindowId window) noexcept;

        [[nodiscard]] DockWorkspace *workspace(Platform::Windowing::WindowId window) const noexcept;

        /// Transactional existing-window transfer: target insertion happens first from a descriptor
        /// copy; origin removal follows only after acceptance succeeds.
        bool transfer_panel(Platform::Windowing::WindowId origin_window,
                            Platform::Windowing::WindowId target_window,
                            const DockPanelId &panel,
                            std::optional<DockPlacement> placement = std::nullopt);

        /// Converts a release-outside event into a deferred spawn request. The panel remains in the
        /// origin workspace until resolve_completion() observes a successful window spawn and target
        /// workspace creation.
        [[nodiscard]] WindowRequestId request_tear_off(
            Platform::Windowing::WindowId origin_window,
            const UI::Docking::DockTearOffRequest &request,
            const Platform::Windowing::WindowConfig &window_config,
            WindowRequests &window_requests,
            Platform::Windowing::WindowFactory factory = nullptr);

        /// Returns true when `completion` belonged to this coordinator. Spawn failure, target creation
        /// failure, or target rejection all leave the origin unchanged.
        bool resolve_completion(const WindowRequestCompletion &completion,
                                const SpawnedWorkspaceResolver &resolve_spawned_workspace);

        /// Queue one close for each empty non-primary workspace. Repeated calls are idempotent until a
        /// close completion unregisters the workspace.
        void request_empty_window_closes(WindowRequests &window_requests);

        [[nodiscard]] usize pending_spawn_count() const noexcept;

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
