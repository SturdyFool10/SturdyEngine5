/// With engine_managed_camera_history the engine supplies each surface's previous-frame
/// view-projection itself: the caller builds a fresh Camera every frame and never commits.

#include <Async/Scheduler.hpp>
#include <Engine/EngineModule.hpp>

#include <iostream>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
} // namespace

int main() {
    using namespace SFT;
    Engine::Engine engine;
    const Core::RenderSurfaceHandle surface{.window_id = WindowManager::WindowId{3}};

    auto current = [](const Engine::PreparedRenderFrame &frame) { return frame.camera.projection * frame.camera.view; };

    auto frame_with_camera_at = [&](float x) {
        Engine::RenderFrameParameters parameters;
        parameters.camera.set_position({x, 0.0f, 0.0f});
        parameters.engine_managed_camera_history = true;
        return engine.prepare_render_frame(surface, Core::FrameInput{}, parameters);
    };

    const auto first = frame_with_camera_at(0.0f);
    check(first.camera.previous_view_projection == current(first), "the first frame has no history: previous equals current");

    const auto second = frame_with_camera_at(1.0f);
    check(second.camera.previous_view_projection == current(first), "the second frame's history is the first frame's view-projection");
    check(!(second.camera.previous_view_projection == current(second)), "a moved camera has distinct previous and current matrices");

    engine.reset_camera_history(surface);
    const auto third = frame_with_camera_at(2.0f);
    check(third.camera.previous_view_projection == current(third), "after a reset the history is gone");

    // Default behaviour is untouched: without the flag the engine keeps nothing.
    Engine::RenderFrameParameters unmanaged;
    unmanaged.camera.set_position({5.0f, 0.0f, 0.0f});
    const auto plain = engine.prepare_render_frame(surface, Core::FrameInput{}, unmanaged);
    check(plain.camera.previous_view_projection == current(plain), "an unmanaged frame uses the camera's own (empty) history");

    Async::Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
