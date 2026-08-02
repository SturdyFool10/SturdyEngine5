#include <Engine/Docking/DockWindowCoordinator.hpp>

#include <cassert>

int main() {
    using namespace SFT;
    using namespace Engine;
    using namespace UI::Docking;
    using Platform::Windowing::WindowId;

    DockWorkspace origin{UString{"origin"}};
    DockWorkspace target{UString{"target"}};
    assert(origin.add_panel(DockPanelDesc{.id = UString{"inspector"}, .title = UString{"Inspector"}, .closable = false}));

    DockWindowCoordinator coordinator;
    assert(coordinator.register_workspace(WindowId{1}, origin, true, false));
    assert(coordinator.register_workspace(WindowId{2}, target, false, true));
    assert(coordinator.transfer_panel(WindowId{1}, WindowId{2}, UString{"inspector"}));
    assert(!origin.has_panel(UString{"inspector"}));
    assert(target.has_panel(UString{"inspector"}));
    assert(target.panel_desc(UString{"inspector"})->title == UString{"Inspector"});
    assert(!target.panel_desc(UString{"inspector"})->closable);

    std::optional<DockPanelDesc> returned = target.take_panel(UString{"inspector"});
    assert(returned.has_value());
    assert(origin.accept_panel(*returned));

    WindowRequests requests;
    Platform::Windowing::WindowConfig config{.title = "Inspector"};
    const WindowRequestId request_id = coordinator.request_tear_off(
        WindowId{1},
        DockTearOffRequest{.panel = UString{"inspector"}, .workspace_local_drop_position = {50.0f, 50.0f}},
        config,
        requests);
    assert(request_id);
    assert(coordinator.pending_spawn_count() == 1);
    assert(origin.has_panel(UString{"inspector"}));

    assert(coordinator.resolve_completion(WindowRequestCompletion{
        .id = request_id,
        .kind = WindowRequestKind::Spawn,
        .accepted = false,
        .message = "simulated failure",
    }, {}));
    assert(origin.has_panel(UString{"inspector"}));
    assert(coordinator.pending_spawn_count() == 0);

    const WindowRequestId successful_request = coordinator.request_tear_off(
        WindowId{1},
        DockTearOffRequest{.panel = UString{"inspector"}, .workspace_local_drop_position = {80.0f, 40.0f}},
        config,
        requests);
    DockWorkspace detached{UString{"detached"}};
    assert(coordinator.resolve_completion(WindowRequestCompletion{
        .id = successful_request,
        .kind = WindowRequestKind::Spawn,
        .accepted = true,
        .surface = Core::RenderSurfaceHandle{WindowId{3}},
        .window = WindowId{3},
    }, [&detached](Core::RenderSurfaceHandle, const DockPanelDesc &) { return &detached; }));
    assert(!origin.has_panel(UString{"inspector"}));
    assert(detached.has_panel(UString{"inspector"}));
    assert(detached.panel_desc(UString{"inspector"})->title == UString{"Inspector"});

    detached.remove_panel(UString{"inspector"});
    coordinator.request_empty_window_closes(requests);
    const std::vector<WindowRequest> close_requests = requests.drain();
    bool found_close = false;
    for (const WindowRequest &request : close_requests) {
        if (const auto *close = std::get_if<CloseWindowRequest>(&request); close != nullptr && close->window == WindowId{3}) {
            found_close = true;
        }
    }
    assert(found_close);
    return 0;
}
