/// Exercises the engine-control C ABI (frame mode, redraw, synthetic input, drag-and-drop, render health, device enumeration and
/// switching) against a headless `Engine`: no window or graphics device, so everything that needs a GPU is checked for its
/// argument validation and its "nothing to report" behaviour only.

#include <cstdio>
#include <cstring>
#include <string>

#include <Async/Scheduler.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "EngineControlFfiTest: %s\n", description);
            ++failures;
        }
    }

} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    const SturdyEngine engine{engine_handle.token()};
    const SturdySurface window{1};

    // Frame mode round-trips and refuses nonsense.
    SturdyFrameMode mode = STURDY_FRAME_MODE_ON_DEMAND;
    check(sturdy_engine_frame_mode(engine, &mode) == STURDY_OK && mode == STURDY_FRAME_MODE_CONTINUOUS, "an engine starts in continuous mode");
    check(sturdy_engine_set_frame_mode(engine, STURDY_FRAME_MODE_ON_DEMAND) == STURDY_OK, "on-demand mode can be set");
    check(sturdy_engine_frame_mode(engine, &mode) == STURDY_OK && mode == STURDY_FRAME_MODE_ON_DEMAND, "the mode reads back");
    check(sturdy_engine_set_frame_mode(engine, STURDY_FRAME_MODE_FORCE_U32) == STURDY_ERROR_INVALID_ARGUMENT, "a bogus mode is refused");
    check(sturdy_engine_request_redraw(engine) == STURDY_OK, "a redraw can be requested");
    check(sturdy_engine_request_redraw_after(engine, 5'000'000) == STURDY_OK, "a delayed redraw can be requested");
    check(sturdy_engine_request_redraw_after(engine, ~uint64_t{0}) == STURDY_OK, "an enormous delay is clamped, not overflowed");
    check(sturdy_engine_frame_mode(SturdyEngine{0}, &mode) != STURDY_OK, "a null engine handle is refused");

    // Synthetic input reaches InputState on the next update.
    check(sturdy_input_inject_key(engine, window, static_cast<int32_t>(SFT::WindowManager::KeyboardKey::Space), STURDY_TRUE, 0, STURDY_FALSE) == STURDY_OK,
          "a key can be injected");
    check(sturdy_input_inject_text(engine, window, "hi") == STURDY_OK, "text can be injected");
    check(sturdy_input_inject_text(engine, window, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "null text is refused");
    check(sturdy_input_inject_mouse_move(engine, window, 12.0f, 34.0f, 0.0f, 0.0f, 0) == STURDY_OK, "a mouse move can be injected");
    check(sturdy_input_inject_mouse_move(engine, window, NAN, 0.0f, 0.0f, 0.0f, 0) == STURDY_ERROR_INVALID_ARGUMENT, "a NaN coordinate is refused");
    check(sturdy_input_inject_mouse_button(engine, window, STURDY_MOUSE_BUTTON_LEFT, STURDY_TRUE, 12.0f, 34.0f, 1) == STURDY_OK, "a mouse button can be injected");
    check(sturdy_input_inject_mouse_button(engine, window, STURDY_MOUSE_BUTTON_FORCE_U32, STURDY_TRUE, 0.0f, 0.0f, 1) == STURDY_ERROR_INVALID_ARGUMENT,
          "an unknown button is refused");
    check(sturdy_input_inject_mouse_wheel(engine, window, 0.0f, 1.0f, 12.0f, 34.0f) == STURDY_OK, "a wheel scroll can be injected");
    engine_object.update(1.0 / 60.0);
    SturdyBool down = STURDY_FALSE;
    check(sturdy_engine_key_down(engine, static_cast<int32_t>(SFT::WindowManager::KeyboardKey::Space), &down) == STURDY_OK && down, "the injected key is held");
    float x = 0.0f, y = 0.0f;
    check(sturdy_engine_mouse_position(engine, &x, &y) == STURDY_OK && x == 12.0f && y == 34.0f, "the injected pointer position arrives");
    check(sturdy_engine_mouse_down(engine, STURDY_MOUSE_BUTTON_LEFT, &down) == STURDY_OK && down, "the injected button is held");

    // Drag-and-drop steps pop one at a time, in order.
    SFT::WindowManager::WindowEvent drop;
    drop.kind = SFT::WindowManager::WindowEventKind::DropBegin;
    engine_object.queue_drop_event(SFT::WindowManager::WindowId{1}, drop);
    drop.kind = SFT::WindowManager::WindowEventKind::DropPosition;
    drop.position = {40, 50};
    engine_object.queue_drop_event(SFT::WindowManager::WindowId{1}, drop);
    drop.kind = SFT::WindowManager::WindowEventKind::DropComplete;
    engine_object.queue_drop_event(SFT::WindowManager::WindowId{1}, drop);
    SturdyDropEvent event{};
    char data[8] = {};
    check(sturdy_engine_next_drop_event(engine, &event, data, sizeof(data), nullptr) == STURDY_OK && event.kind == STURDY_DROP_BEGIN && event.window == 1, "the drop begins first");
    check(sturdy_engine_next_drop_event(engine, &event, nullptr, 0, nullptr) == STURDY_OK && event.kind == STURDY_DROP_HOVER && event.x == 40.0f && event.y == 50.0f,
          "the hover follows with its position");
    check(sturdy_engine_next_drop_event(engine, &event, nullptr, 0, nullptr) == STURDY_OK && event.kind == STURDY_DROP_COMPLETE, "the drop completes last");
    check(sturdy_engine_next_drop_event(engine, &event, nullptr, 0, nullptr) == STURDY_ERROR_NOT_AVAILABLE, "an empty queue says so");

    // Health starts clean.
    SturdyRenderHealth health{};
    check(sturdy_engine_render_health(engine, &health) == STURDY_OK && health.frames_failed == 0 && health.consecutive_failures == 0, "no frame has failed yet");
    char message[16] = {};
    size_t length = 99;
    check(sturdy_engine_render_health_last_error(engine, message, sizeof(message), &length) == STURDY_OK && length == 1 && message[0] == '\0', "there is no last error");

    // Without a device the inventory is empty but the queries still answer.
    uint64_t generation = 99;
    check(sturdy_engine_device_generation(engine, &generation) == STURDY_OK, "the device generation can be read");
    uint32_t backend_count = 99;
    check(sturdy_engine_available_backends(engine, nullptr, 0, &backend_count) == STURDY_OK, "the backend count can be read");
    uint32_t adapter_count = 99;
    check(sturdy_engine_adapter_count(engine, &adapter_count) == STURDY_OK, "the adapter count can be read");
    SturdyAdapterDescription adapter{};
    check(sturdy_engine_adapter_description(engine, adapter_count, &adapter) == STURDY_ERROR_OUT_OF_RANGE, "an adapter index past the end is out of range");
    SturdyBackendOffer offer{};
    check(sturdy_engine_adapter_backend(engine, adapter_count, 0, &offer) == STURDY_ERROR_OUT_OF_RANGE, "a backend of a missing adapter is out of range");
    if (adapter_count > 0) {
        check(sturdy_engine_adapter_description(engine, 0, &adapter) == STURDY_OK && adapter.name[0] != '\0' && adapter.stable_id[0] != '\0', "the first adapter is described");
        check(sturdy_engine_adapter_backend(engine, 0, 0, &offer) == STURDY_OK, "its first backend is described");
    }

    SturdyDeviceRequest request{};
    check(sturdy_device_request_init(&request) == STURDY_OK && request.struct_size == sizeof(request), "a device request initialises");
    SturdyDeviceSwitchResult result{};
    request.struct_size = 1;
    check(sturdy_engine_switch_device(engine, &request, &result) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong request size is refused");
    check(sturdy_device_request_init(&request) == STURDY_OK, "the request re-initialises");
    request.backend = STURDY_BACKEND_FORCE_U32;
    check(sturdy_engine_switch_device(engine, &request, &result) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown backend is refused");
    SturdyDeviceChangeEvent change{};
    check(sturdy_engine_next_device_change(engine, &change) == STURDY_ERROR_NOT_AVAILABLE, "no device change has happened");

    // Debug draw: shapes accumulate, bad input is refused, the master switch and clear work.
    SturdyBool enabled = STURDY_FALSE;
    check(sturdy_debug_draw_enabled(engine, &enabled) == STURDY_OK && enabled, "debug draw starts enabled");
    const SturdyVec4 red{1, 0, 0, 1};
    check(sturdy_debug_draw_line(engine, {0, 0, 0}, {1, 1, 1}, red, 5.0f) == STURDY_OK, "a line can be drawn");
    check(sturdy_debug_draw_sphere(engine, {0, 1, 0}, 0.5f, red, 5.0f) == STURDY_OK, "a sphere can be drawn");
    check(sturdy_debug_draw_box(engine, {0, 0, 0}, {1, 1, 1}, red, 5.0f) == STURDY_OK, "a box can be drawn");
    const SturdyVec3 path[3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    check(sturdy_debug_draw_polyline(engine, path, 3, red, STURDY_TRUE, 5.0f) == STURDY_OK, "a polyline can be drawn");
    SturdyMat4 identity{};
    identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
    check(sturdy_debug_draw_axes(engine, identity, 1.0f, 5.0f) == STURDY_OK, "axes can be drawn");
    check(sturdy_debug_draw_grid(engine, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 4, 1.0f, red, 5.0f) == STURDY_OK, "a grid can be drawn");
    uint64_t lines = 0;
    check(sturdy_debug_draw_line_count(engine, &lines) == STURDY_OK && lines > 20, "the shapes became segments");
    check(sturdy_debug_draw_line(engine, {NAN, 0, 0}, {1, 1, 1}, red, 0.0f) == STURDY_ERROR_INVALID_ARGUMENT, "a NaN point is refused");
    check(sturdy_debug_draw_line(engine, {0, 0, 0}, {1, 1, 1}, red, -1.0f) == STURDY_ERROR_INVALID_ARGUMENT, "a negative lifetime is refused");
    check(sturdy_debug_draw_grid(engine, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}, 100000, 1.0f, red, 0.0f) == STURDY_ERROR_INVALID_ARGUMENT, "an enormous grid is refused");
    SturdyDebugDrawSettings draw_settings{};
    check(sturdy_debug_draw_settings_init(&draw_settings) == STURDY_OK, "draw settings initialise");
    draw_settings.occluded_alpha = 0.5f;
    check(sturdy_debug_draw_set_settings(engine, &draw_settings) == STURDY_OK, "draw settings can be set");
    SturdyDebugDrawSettings read_back{};
    check(sturdy_debug_draw_settings(engine, &read_back) == STURDY_OK && read_back.occluded_alpha == 0.5f, "draw settings read back");
    check(sturdy_debug_draw_clear(engine) == STURDY_OK && sturdy_debug_draw_line_count(engine, &lines) == STURDY_OK && lines == 0, "clear removes everything");
    check(sturdy_debug_draw_set_enabled(engine, STURDY_FALSE) == STURDY_OK && sturdy_debug_draw_line(engine, {0, 0, 0}, {1, 1, 1}, red, 5.0f) == STURDY_OK &&
              sturdy_debug_draw_line_count(engine, &lines) == STURDY_OK && lines == 0,
          "a disabled debug draw ignores shapes");

    // Update hooks and deferred actions run on the updating thread, in order, and release their user data.
    struct HookState {
        int hook_runs = 0;
        int deferred_runs = 0;
        int released = 0;
        double last_delta = 0.0;
    } hooks;
    uint64_t hook_id = 0;
    check(sturdy_engine_add_update_hook(
              engine,
              [](SturdyEngine, double delta, void *user) {
                  auto *state = static_cast<HookState *>(user);
                  ++state->hook_runs;
                  state->last_delta = delta;
              },
              &hooks, [](void *user) { ++static_cast<HookState *>(user)->released; }, &hook_id) == STURDY_OK && hook_id != 0,
          "an update hook is registered");
    check(sturdy_engine_defer(
              engine, [](SturdyEngine, void *user) { ++static_cast<HookState *>(user)->deferred_runs; }, &hooks, nullptr) == STURDY_OK,
          "an action is deferred");
    check(hooks.deferred_runs == 0 && hooks.hook_runs == 0, "nothing runs before the update");
    engine_object.update(0.25);
    check(hooks.hook_runs == 1 && hooks.deferred_runs == 1 && hooks.last_delta > 0.0, "the hook and the deferred action ran in the update");
    engine_object.update(0.25);
    check(hooks.hook_runs == 2 && hooks.deferred_runs == 1, "the hook runs every update, the deferred action once");
    check(sturdy_engine_remove_update_hook(engine, hook_id) == STURDY_OK, "the hook is removed");
    check(hooks.released == 1, "removing the hook released its user data");
    check(sturdy_engine_remove_update_hook(engine, hook_id) == STURDY_ERROR_NOT_AVAILABLE, "removing twice reports an unknown id");
    engine_object.update(0.25);
    check(hooks.hook_runs == 2, "a removed hook does not run");

    // A headless UI tree is laid out by the update, its builder using the ordinary element calls.
    struct TreeState {
        int builds = 0;
        int released = 0;
        float width = 0.0f;
        SturdyResult element_result = STURDY_OK;
    } tree_state;
    SturdyUiTreeDesc tree_desc{};
    check(sturdy_ui_tree_desc_init(&tree_desc) == STURDY_OK && tree_desc.struct_size == sizeof(tree_desc), "a tree description initialises");
    tree_desc.name = "test";
    tree_desc.target.kind = STURDY_UI_TREE_TARGET_HEADLESS;
    tree_desc.target.headless_width = 320;
    tree_desc.target.headless_height = 200;
    tree_desc.user = &tree_state;
    tree_desc.release_user = [](void *user) { ++static_cast<TreeState *>(user)->released; };
    SturdyEntity tree{};
    check(sturdy_ui_tree_spawn(engine, &tree_desc, &tree) == STURDY_ERROR_INVALID_ARGUMENT, "a tree without a builder is refused");
    tree_desc.builder = [](SturdyEngine e, SturdyUiTreeFrame *frame, void *user) {
        auto *state = static_cast<TreeState *>(user);
        ++state->builds;
        state->width = frame->width;
        SturdyUiElement element{};
        element.struct_size = sizeof(element);
        element.width_kind = STURDY_UI_SIZING_FIXED;
        element.width_value = 100.0f;
        element.height_kind = STURDY_UI_SIZING_FIXED;
        element.height_value = 50.0f;
        state->element_result = sturdy_ui_begin_element(e, &element);
        if (state->element_result == STURDY_OK) {
            (void)sturdy_ui_end_element(e);
        }
    };
    check(sturdy_ui_tree_spawn(engine, &tree_desc, &tree) == STURDY_OK, "a headless tree is spawned");
    engine_object.update(1.0 / 60.0);
    check(tree_state.builds == 1 && tree_state.width == 320.0f, "the tree is built at its headless size");
    check(tree_state.element_result == STURDY_OK, "the builder can open elements through the ordinary calls");
    SturdyUiTreeStats tree_stats{};
    check(sturdy_ui_tree_stats(engine, tree, &tree_stats) == STURDY_OK && tree_stats.builds == 1 && tree_stats.failed_builds == 0, "the stats count the build");
    check(sturdy_ui_tree_request_rebuild(engine, tree) == STURDY_OK, "a rebuild can be requested");
    check(sturdy_ui_tree_set_rebuild_policy(engine, tree, STURDY_UI_REBUILD_ON_DIRTY, 0.0f) == STURDY_OK, "the policy can change");
    check(sturdy_ui_tree_set_layer(engine, tree, 3) == STURDY_OK, "the layer can change");
    int32_t layer = 0;
    check(sturdy_ui_tree_layer(engine, tree, &layer) == STURDY_OK && layer == 3, "the layer reads back");
    SturdyUiTreeTarget target{};
    check(sturdy_ui_tree_target(engine, tree, &target) == STURDY_OK && target.kind == STURDY_UI_TREE_TARGET_HEADLESS && target.headless_width == 320, "the target reads back");
    check(sturdy_ui_tree_point_at(engine, tree, STURDY_TRUE, 0.5f, 0.5f, STURDY_TRUE) == STURDY_OK, "the tree can be pointed at");
    check(sturdy_ui_tree_point_at(engine, tree, STURDY_FALSE, 0.0f, 0.0f, STURDY_FALSE) == STURDY_OK, "the pointer can leave the tree");
    check(sturdy_ui_tree_add_text(engine, tree, "x") == STURDY_OK, "text can be typed");
    char error_text[8] = {};
    size_t error_length = 99;
    check(sturdy_ui_tree_last_error(engine, tree, error_text, sizeof(error_text), &error_length) == STURDY_OK && error_length == 1, "a healthy tree has no error");
    uint64_t tree_count = 0;
    check(sturdy_ui_tree_count(engine, &tree_count) == STURDY_OK, "the tree count can be read");
    check(sturdy_ui_tree_stats(engine, SturdyEntity{12345, 7}, &tree_stats) == STURDY_ERROR_COMPONENT_MISSING, "an entity with no tree is reported");
    check(sturdy_ui_tree_render_targets(engine, nullptr) == STURDY_OK, "rendering off-screen trees with none is fine");

    // Without a graphics device: render targets and settings report cleanly rather than crash.
    SturdyRenderTargetDesc target_desc{};
    check(sturdy_render_target_desc_init(&target_desc) == STURDY_OK && target_desc.struct_size == sizeof(target_desc) && target_desc.mip_levels == STURDY_RENDER_TARGET_AUTO_MIPS,
          "a render target description initialises with auto mips");
    SturdyRenderTarget render_target{};
    target_desc.width = 64;
    target_desc.height = 64;
    check(sturdy_render_target_create(engine, &target_desc, &render_target) != STURDY_OK, "no render target without a device");
    SturdyRenderTargetDesc described{};
    check(sturdy_render_target_description(engine, SturdyRenderTarget{99}, &described) == STURDY_ERROR_NOT_AVAILABLE, "an unknown target is not available");
    check(sturdy_render_target_resize(engine, SturdyRenderTarget{99}, 0, 10) == STURDY_ERROR_INVALID_ARGUMENT, "a zero size is refused");
    target_desc.struct_size = 4;
    check(sturdy_render_target_create(engine, &target_desc, &render_target) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong description size is refused");
    SturdyPresentTimingFeedback timing{};
    check(sturdy_engine_present_timing_feedback(engine, window, &timing) == STURDY_OK && !timing.available, "no timing is available without a surface");

    SFT::Async::Scheduler::shutdown();
    check(tree_state.released <= 1 && hooks.released == 1, "user data is released at most once");
    if (failures == 0) {
        std::printf("EngineControlFfiTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
