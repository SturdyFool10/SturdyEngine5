/// UI trees are entities laid out by `Engine::update`, independently of each other and of any window frame callback. Runs on the CPU
/// only: no device, so trees lay out and route input but their overlays draw nothing.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/UiTree.hpp>

#include <stdexcept>
#include <string_view>

namespace {
    using namespace SFT;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            Foundation::log_error("FAILED: {}", message);
            ++failures;
        }
    }

    UString id(std::string_view text) { return UString{text}; }

    /// A tree with one square box of `size` at the top-left, named `box`.
    void draw_box(UI::Context &ui, std::string_view box, f32 size) {
        auto root = ui.element(UI::ElementDecl{});
        auto element = ui.element(UI::ElementDecl{
            .sizing = {UI::SizingAxis::fixed(size), UI::SizingAxis::fixed(size)},
            .background_color = UI::Color{0.2, 0.4, 0.8, 1.0},
            .id = id(box),
        });
    }

    struct Clicks {
        int hud = 0;
        int popup = 0;
    };
} // namespace

int main() {
    Engine::Engine engine;
    const WindowManager::WindowId window{1};
    Engine::WindowSnapshot snapshot{};
    snapshot.id = window;
    snapshot.size = {800u, 600u};
    snapshot.framebuffer_size = {800u, 600u};
    engine.window_state().sync({snapshot}, window);

    Clicks clicks;
    int monitor_builds = 0;
    int flaky_builds = 0;

    // Two trees on the same window: a HUD with a 200 px box, and a popup above it (higher layer) with a 100 px box over the HUD's.
    engine.spawn_ui_tree({.name = "hud", .builder = [&](Engine::UiTreeFrame &frame) {
                              draw_box(frame.ui, "hud_box", 200.0f);
                              if (frame.ui.clicked(id("hud_box"))) ++clicks.hud;
                          }});
    engine.spawn_ui_tree({.name = "popup", .builder = [&](Engine::UiTreeFrame &frame) {
                              draw_box(frame.ui, "popup_box", 100.0f);
                              if (frame.ui.clicked(id("popup_box"))) ++clicks.popup;
                          },
                          .layer = 1});
    // A monitor in the world (headless here: no device to render to), rebuilt only when something changes.
    const Ecs::Entity monitor_entity = engine.spawn_ui_tree({.name = "monitor",
                                                             .target = Engine::UiHeadlessTarget{{256u, 256u}},
                                                             .builder = [&](Engine::UiTreeFrame &frame) {
                                                                 ++monitor_builds;
                                                                 draw_box(frame.ui, "screen", 64.0f);
                                                             },
                                                             .rebuild = Engine::UiRebuildPolicy::OnDirty});
    // A tree whose builder throws on its second build.
    const Ecs::Entity flaky_entity = engine.spawn_ui_tree({.name = "flaky",
                                                           .target = Engine::UiHeadlessTarget{{64u, 64u}},
                                                           .builder = [&](Engine::UiTreeFrame &frame) {
                                                               draw_box(frame.ui, "flaky_box", 16.0f);
                                                               if (++flaky_builds == 2) throw std::runtime_error("builder failed");
                                                           }});

    const auto tree_of = [&](Ecs::Entity entity) { return *engine.ecs_world().get_component<Engine::UiTree>(entity); };

    engine.update(1.0 / 60.0);
    check(engine.ui_trees().tree_count() == 4, "every UiTree entity must be picked up by the update");
    check(monitor_builds == 1, "an OnDirty tree must be laid out once to have a first frame");
    const Engine::UiTree monitor = tree_of(monitor_entity);
    check(monitor.last_extent() == glm::vec2{256.0f, 256.0f}, "a headless tree must lay out at its own extent");

    // The flaky builder throws now: its previous frame stays, the failure is counted, and nothing else is disturbed.
    engine.update(1.0 / 60.0);
    const Engine::UiTree flaky = tree_of(flaky_entity);
    check(flaky.stats().failed_builds == 1, "a builder that throws must be counted as a failed build");
    check(flaky.last_extent() == glm::vec2{64.0f, 64.0f}, "a failed build must keep the tree's previous frame");
    check(monitor_builds == 1, "an OnDirty tree with nothing new must not be laid out again");
    check(monitor.stats().skipped >= 1, "a skipped build must be counted");
    engine.update(1.0 / 60.0);
    check(flaky.stats().builds == 2 && flaky.stats().failed_builds == 1, "a tree must recover on the build after a failure");

    // Pointing at the monitor (from a world ray, say) is input: it marks the tree dirty and it is laid out again.
    tree_of(monitor_entity).point_at(glm::vec2{0.1f, 0.1f}, false);
    engine.update(1.0 / 60.0);
    check(monitor_builds == 2, "input to an OnDirty tree must lay it out again");
    tree_of(monitor_entity).request_rebuild();
    engine.update(1.0 / 60.0);
    check(monitor_builds == 3, "request_rebuild must lay an OnDirty tree out again");

    // Where both boxes overlap, the popup (higher layer) takes the press; the HUD below never sees it.
    engine.inject_mouse_move(window, 50.0f, 50.0f);
    engine.update(1.0 / 60.0);
    engine.inject_mouse_button(window, WindowManager::MouseButton::Left, true, 50.0f, 50.0f);
    engine.update(1.0 / 60.0);
    engine.inject_mouse_button(window, WindowManager::MouseButton::Left, false, 50.0f, 50.0f);
    engine.update(1.0 / 60.0);
    check(clicks.popup == 1, "a press over the topmost tree must reach it");
    check(clicks.hud == 0, "a press taken by the tree above must not reach the tree below");

    // Outside the popup's box but inside the HUD's, the HUD gets it.
    engine.inject_mouse_move(window, 150.0f, 150.0f);
    engine.update(1.0 / 60.0);
    engine.inject_mouse_button(window, WindowManager::MouseButton::Left, true, 150.0f, 150.0f);
    engine.update(1.0 / 60.0);
    engine.inject_mouse_button(window, WindowManager::MouseButton::Left, false, 150.0f, 150.0f);
    engine.update(1.0 / 60.0);
    check(clicks.hud == 1, "a press outside the upper tree's elements must fall through to the tree below");
    check(clicks.popup == 1, "the upper tree must not take a press outside its elements");

    // A frame at a size the trees were not laid out at (the window was resized after the update read it): each window tree is laid
    // out again at the frame's size before its overlay is attached, so no overlay is ever a mismatched size.
    std::vector<Renderer::OverlayPass> overlays;
    engine.ui_trees().append_window_overlays(engine, Core::RenderSurfaceHandle{window}, glm::uvec2{640u, 480u}, overlays);
    check(overlays.size() == 2, "both window trees must be attached to the window's frame (and the headless ones must not)");
    check(overlays.size() == 2 && overlays[0].name == "hud" && overlays[1].name == "popup", "window trees must stack lowest layer first");

    // Ending a tree is destroying its entity.
    engine.ecs_world().destroy(flaky_entity);
    engine.update(1.0 / 60.0);
    check(engine.ui_trees().tree_count() == 3, "a destroyed tree entity must stop being laid out");

    Async::Scheduler::shutdown();
    if (failures != 0) {
        Foundation::log_error("{} check(s) failed", failures);
        return 1;
    }
    Foundation::log_info("UiTreeTest passed");
    return 0;
}
