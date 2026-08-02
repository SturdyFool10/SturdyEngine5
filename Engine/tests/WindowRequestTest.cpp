#include <Engine/WindowRequests.hpp>

#include <cassert>
#include <string>
#include <variant>

int main() {
    using namespace SFT::Engine;
    using namespace SFT::Platform::Windowing;

    WindowRequests requests;
    std::string temporary_title = "Detached Inspector";
    WindowConfig config{
        .title = temporary_title.c_str(),
        .extent = {640, 480},
        .position = {120, 80},
        .use_default_position = false,
    };

    const WindowRequestId spawn_id = requests.spawn(config);
    temporary_title.assign("overwritten");
    const WindowRequestId close_id = requests.close(WindowId{42});
    assert(spawn_id);
    assert(close_id);
    assert(spawn_id != close_id);
    assert(requests.has_pending());

    std::vector<WindowRequest> pending = requests.drain();
    assert(pending.size() == 2);
    assert(!requests.has_pending());

    const SpawnWindowRequest &spawn = std::get<SpawnWindowRequest>(pending[0]);
    const WindowConfig owned_view = spawn.window.view();
    assert(std::string{owned_view.title} == "Detached Inspector");
    assert(owned_view.extent.x == 640);
    assert(owned_view.position.x == 120);

    const CloseWindowRequest &close = std::get<CloseWindowRequest>(pending[1]);
    assert(close.window == WindowId{42});

    requests.complete(WindowRequestCompletion{
        .id = spawn_id,
        .kind = WindowRequestKind::Spawn,
        .accepted = false,
        .message = "test failure",
    });
    std::vector<WindowRequestCompletion> completions = requests.take_completions();
    assert(completions.size() == 1);
    assert(completions[0].id == spawn_id);
    assert(!completions[0].accepted);
    assert(completions[0].message == "test failure");
    assert(requests.take_completions().empty());
    return 0;
}
