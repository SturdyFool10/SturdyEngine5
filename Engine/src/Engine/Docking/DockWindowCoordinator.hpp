#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/WindowRequests.hpp>
#include <UI/Docking/Docking.hpp>

#include <functional>
#include <optional>
#include <unordered_map>

namespace SFT::Engine {


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


        using SpawnedWorkspaceResolver =
            std::function<DockWorkspace *(Core::RenderSurfaceHandle, const DockPanelDesc &)>;

        /// Registers workspace using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        /// @param workspace `workspace` value used by the operation.
        /// @param primary `primary` value used by the operation.
        /// @param close_when_empty `close_when_empty` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool register_workspace(Platform::Windowing::WindowId window, DockWorkspace &workspace,
                                bool primary = false, bool close_when_empty = true);

        /// Unregisters workspace using the supplied arguments and current state.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void unregister_workspace(Platform::Windowing::WindowId window) noexcept;

        /// Performs the workspace operation for `DockWindowCoordinator` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DockWorkspace *workspace(Platform::Windowing::WindowId window) const noexcept;


        /// Performs the transfer panel operation for `DockWindowCoordinator` using the supplied arguments.
        ///
        /// @param origin_window Window used or affected by the operation.
        /// @param target_window Window used or affected by the operation.
        /// @param panel `panel` value used by the operation.
        /// @param placement `placement` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        bool transfer_panel(Platform::Windowing::WindowId origin_window,
                            Platform::Windowing::WindowId target_window,
                            const DockPanelId &panel,
                            std::optional<DockPlacement> placement = std::nullopt);


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
        [[nodiscard]] WindowRequestId request_tear_off(
            Platform::Windowing::WindowId origin_window,
            const UI::Docking::DockTearOffRequest &request,
            const Platform::Windowing::WindowConfig &window_config,
            WindowRequests &window_requests,
            Platform::Windowing::WindowFactory factory = nullptr);


        /// Resolves completion into the concrete value used by the engine.
        ///
        /// @param completion `completion` value used by the operation.
        /// @param resolve_spawned_workspace `resolve_spawned_workspace` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool resolve_completion(const WindowRequestCompletion &completion,
                                const SpawnedWorkspaceResolver &resolve_spawned_workspace);


        /// Requests empty window closes using the supplied arguments and current state.
        ///
        /// @param window_requests Window used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void request_empty_window_closes(WindowRequests &window_requests);

        /// Returns the pending spawn count for this `DockWindowCoordinator`.
        ///
        /// @return Returns the current pending spawn count value.
        /// @note This function does not throw exceptions.
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
