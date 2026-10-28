#include <Engine/UiTree.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <Engine/EcsEvents.hpp>
#include <Engine/EngineModule.hpp>
#include <Engine/RenderGraph.hpp>
#include <Engine/ScreenUi.hpp>
#include <Engine/WindowState.hpp>
#include <Foundation/ThrottledLog.hpp>
#include <Renderer/UI/UiWorld.hpp>

namespace SFT::Engine {

    struct UiTreeState {
        struct Inner {
            UiTreeDesc desc;
            UI::UiSurface surface;
            /// The latest finished frame; what the renderer draws until a newer one replaces it.
            std::shared_ptr<UI::FrameSnapshot> snapshot;
            glm::vec2 snapshot_extent{0.0f};
            /// Bumped per finished frame; an off-screen target is re-rendered only when this moved past `rendered_serial`.
            u64 snapshot_serial = 0;
            u64 rendered_serial = 0;
            u64 target_frame_index = 0;
            /// Seconds since the last build (what the builder sees as its delta).
            f64 since_build = 0.0;
            /// Seconds since the last build, for `FixedRate`.
            f64 since_rate_tick = 0.0;
            UiTreeStats stats;
        };

        Async::Mutex<Inner> inner;
        /// Set by input, `request_rebuild`, a builder's `request_rebuild`, and property changes; cleared by a build.
        std::atomic<bool> dirty{true};
        /// `pointer_consumed` of the last build, readable without the lock (the input router asks for every tree on every event).
        std::atomic<bool> pointer_consumed{false};
        /// Order the tree was created in: stacking among equal layers, newest on top.
        u64 order = 0;
        /// Cached copy of `desc.layer` for sorting without the lock.
        std::atomic<i32> layer{0};
    };

    namespace {

        std::atomic<u64> g_next_tree_order{1};

        /// Depth of tree builds on this thread. A builder that (indirectly) starts another build would interleave two layouts on
        /// one thread's current Clay context, so a nested build is refused.
        thread_local int t_build_depth = 0;

        /// Updates a retired tree is kept for before its GPU objects are destroyed: longer than any frame in flight can live.
        constexpr u32 retire_after_updates = 8;

        [[nodiscard]] std::optional<WindowManager::WindowId> resolve_window(Engine &engine, const UiWindowTarget &target) {
            if (target.window) {
                return target.window;
            }
            if (const WindowSnapshot *primary = engine.window_state().primary()) {
                return primary->id;
            }
            return std::nullopt;
        }

        [[nodiscard]] bool targets_window(Engine &engine, const UiTreeTarget &target, WindowManager::WindowId window) {
            const auto *window_target = std::get_if<UiWindowTarget>(&target);
            if (window_target == nullptr) {
                return false;
            }
            const std::optional<WindowManager::WindowId> resolved = resolve_window(engine, *window_target);
            return resolved && *resolved == window;
        }

        /// The colour format a window tree's UI renderer is first created for. A guess (the display may refuse HDR): the overlay rebuilds
        /// its pipelines when the pass it lands in turns out to be another format (`UiSurface::make_overlay`).
        [[nodiscard]] RHI::Format initial_color_format(Engine &engine) {
            return engine.config().features.presentation.hdr_enabled ? RHI::Format::RGBA16Float : RHI::Format::BGRA8UnormSrgb;
        }

        void deliver(const std::shared_ptr<UiTreeState> &tree, const auto &fn) {
            {
                auto inner = tree->inner.lock();
                fn(inner->surface.input());
            }
            tree->dirty.store(true, std::memory_order_release);
        }

    } // namespace

    // ---- UiTree -----------------------------------------------------------------------------------------------------------

    UiTree::UiTree(UiTreeDesc desc) : state_(std::make_shared<UiTreeState>()) {
        state_->order = g_next_tree_order.fetch_add(1, std::memory_order_relaxed);
        state_->layer.store(desc.layer, std::memory_order_relaxed);
        auto inner = state_->inner.lock();
        inner->desc = std::move(desc);
    }

    void UiTree::set_builder(UiTreeBuilder builder) {
        if (!state_) return;
        state_->inner.lock()->desc.builder = std::move(builder);
        request_rebuild();
    }

    void UiTree::set_target(UiTreeTarget target) {
        if (!state_) return;
        state_->inner.lock()->desc.target = std::move(target);
        request_rebuild();
    }

    UiTreeTarget UiTree::target() const {
        if (!state_) return UiWindowTarget{};
        return state_->inner.lock()->desc.target;
    }

    void UiTree::set_rebuild_policy(UiRebuildPolicy policy, f32 rebuild_hz) {
        if (!state_) return;
        auto inner = state_->inner.lock();
        inner->desc.rebuild = policy;
        inner->desc.rebuild_hz = rebuild_hz;
    }

    void UiTree::set_layer(i32 layer) {
        if (!state_) return;
        state_->inner.lock()->desc.layer = layer;
        state_->layer.store(layer, std::memory_order_relaxed);
    }

    i32 UiTree::layer() const { return state_ ? state_->layer.load(std::memory_order_relaxed) : 0; }

    void UiTree::request_rebuild() noexcept {
        if (state_) {
            state_->dirty.store(true, std::memory_order_release);
        }
    }

    void UiTree::with_input(const std::function<void(UI::UiInput &)> &fn) {
        if (!state_ || !fn) return;
        deliver(state_, fn);
    }

    void UiTree::point_at(std::optional<glm::vec2> uv, bool down) {
        if (!state_) return;
        {
            auto inner = state_->inner.lock();
            UI::UiInput &input = inner->surface.input();
            if (!uv) {
                if (input.pointer().down) {
                    input.cancel_pointer();
                }
            } else {
                input.move_pointer(UI::ui_pixel_from_uv(*uv, inner->snapshot_extent));
                if (input.pointer().down != down) {
                    input.set_pointer_down(down);
                }
            }
        }
        state_->dirty.store(true, std::memory_order_release);
    }

    bool UiTree::pointer_consumed() const { return state_ && state_->pointer_consumed.load(std::memory_order_acquire); }

    UiTreeStats UiTree::stats() const {
        if (!state_) return {};
        return state_->inner.lock()->stats;
    }

    glm::vec2 UiTree::last_extent() const {
        if (!state_) return glm::vec2{0.0f};
        return state_->inner.lock()->snapshot_extent;
    }

    // ---- UiTrees ----------------------------------------------------------------------------------------------------------

    UiTrees::UiTrees() = default;
    UiTrees::~UiTrees() = default;

    std::vector<UiTrees::Entry> UiTrees::snapshot_live() const {
        return shared_.lock()->live;
    }

    usize UiTrees::tree_count() const { return shared_.lock()->live.size(); }

    std::vector<std::shared_ptr<UiTreeState>> UiTrees::input_trees_for(Engine &engine, const std::vector<Entry> &live,
                                                                      WindowManager::WindowId window) {
        std::vector<std::shared_ptr<UiTreeState>> trees;
        for (const Entry &entry : live) {
            auto inner = entry.state->inner.lock();
            if (inner->desc.window_input && targets_window(engine, inner->desc.target, window)) {
                trees.push_back(entry.state);
            }
        }
        std::ranges::sort(trees, [](const auto &a, const auto &b) {
            const i32 la = a->layer.load(std::memory_order_relaxed);
            const i32 lb = b->layer.load(std::memory_order_relaxed);
            return la != lb ? la > lb : a->order > b->order;
        });
        return trees;
    }

    void UiTrees::install(Engine &engine, Ecs::Schedule &schedule) {
        // One system for every event kind, so a press is routed with the same view of the trees as the moves before it. Window events
        // reach every tree on that window as pointer moves (each needs to know where the pointer is to un-hover); presses, releases and
        // scrolling go to one tree; keys and text go to the focused tree.
        router_system_ = schedule.add_system(
            [this, engine_ptr = &engine](Ecs::EventReader<MouseMoveEvent> mouse_move, Ecs::EventReader<MouseButtonEvent> mouse_button,
                                         Ecs::EventReader<MouseWheelEvent> mouse_wheel, Ecs::EventReader<KeyboardEvent> keyboard,
                                         Ecs::EventReader<TextInputEvent> text, Ecs::EventReader<TextEditingEvent> text_editing,
                                         Ecs::EventReader<WindowStateEvent> window_state) noexcept {
                Engine &engine = *engine_ptr;
                if (mouse_move.read().empty() && mouse_button.read().empty() && mouse_wheel.read().empty() && keyboard.read().empty() &&
                    text.read().empty() && text_editing.read().empty() && window_state.read().empty()) {
                    return;
                }
                auto shared = shared_.lock();
                if (shared->live.empty()) {
                    return;
                }
                const auto density = [&engine](WindowManager::WindowId window) {
                    const WindowSnapshot *snapshot = engine.window_state().find(window);
                    return snapshot != nullptr ? snapshot->pixel_density() : glm::vec2{1.0f};
                };
                // The tree under the pointer: the one holding a press, else the topmost one the pointer was over at its last build.
                const auto pointer_tree = [](const std::vector<std::shared_ptr<UiTreeState>> &trees,
                                             const WindowRouting &routing) -> std::shared_ptr<UiTreeState> {
                    if (auto captured = routing.capture.lock()) {
                        return captured;
                    }
                    for (const auto &tree : trees) {
                        if (tree->pointer_consumed.load(std::memory_order_acquire)) {
                            return tree;
                        }
                    }
                    return nullptr;
                };
                // Keys and text: the tree that last took a press, else the topmost one.
                const auto keyboard_tree = [](const std::vector<std::shared_ptr<UiTreeState>> &trees,
                                              const WindowRouting &routing) -> std::shared_ptr<UiTreeState> {
                    if (auto focused = routing.focus.lock()) {
                        if (std::ranges::find(trees, focused) != trees.end()) {
                            return focused;
                        }
                    }
                    return trees.empty() ? nullptr : trees.front();
                };

                try {
                    for (const MouseMoveEvent &event : mouse_move.read()) {
                        for (const auto &tree : input_trees_for(engine, shared->live, event.window)) {
                            deliver(tree, [&](UI::UiInput &input) { apply_window_event(input, event, density(event.window)); });
                        }
                    }
                    for (const MouseButtonEvent &event : mouse_button.read()) {
                        if (event.mouse.button_code != WindowManager::MouseButton::Left) {
                            continue;
                        }
                        const auto trees = input_trees_for(engine, shared->live, event.window);
                        WindowRouting &routing = shared->routing[event.window];
                        const std::shared_ptr<UiTreeState> target = pointer_tree(trees, routing);
                        if (event.action == ButtonAction::Pressed) {
                            routing.capture = target;
                            if (target) {
                                routing.focus = target;
                            }
                        } else {
                            routing.capture.reset();
                        }
                        if (target) {
                            deliver(target, [&](UI::UiInput &input) { apply_window_event(input, event, density(event.window)); });
                        }
                    }
                    for (const MouseWheelEvent &event : mouse_wheel.read()) {
                        const auto trees = input_trees_for(engine, shared->live, event.window);
                        if (const auto target = pointer_tree(trees, shared->routing[event.window])) {
                            deliver(target, [&](UI::UiInput &input) { apply_window_event(input, event); });
                        }
                    }
                    for (const KeyboardEvent &event : keyboard.read()) {
                        const auto trees = input_trees_for(engine, shared->live, event.window);
                        if (const auto target = keyboard_tree(trees, shared->routing[event.window])) {
                            deliver(target, [&](UI::UiInput &input) { apply_window_event(input, event); });
                        }
                    }
                    for (const TextInputEvent &event : text.read()) {
                        const auto trees = input_trees_for(engine, shared->live, event.window);
                        if (const auto target = keyboard_tree(trees, shared->routing[event.window])) {
                            deliver(target, [&](UI::UiInput &input) { apply_window_event(input, event); });
                        }
                    }
                    for (const TextEditingEvent &event : text_editing.read()) {
                        const auto trees = input_trees_for(engine, shared->live, event.window);
                        if (const auto target = keyboard_tree(trees, shared->routing[event.window])) {
                            deliver(target, [&](UI::UiInput &input) { apply_window_event(input, event); });
                        }
                    }
                    // Losing focus mid-drag means the release goes elsewhere: drop the press in every tree on that window.
                    for (const WindowStateEvent &event : window_state.read()) {
                        if (event.kind != WindowManager::WindowEventKind::FocusLost) {
                            continue;
                        }
                        shared->routing[event.window].capture.reset();
                        for (const auto &tree : input_trees_for(engine, shared->live, event.window)) {
                            deliver(tree, [](UI::UiInput &input) {
                                if (input.pointer().down) {
                                    input.cancel_pointer();
                                }
                            });
                        }
                    }
                } catch (const std::exception &error) {
                    Foundation::log_error("UI trees: routing window input failed: {}", error.what());
                }
            });
    }

    bool UiTrees::build_one(Engine &engine, const Entry &entry, glm::vec2 extent, f64 delta_seconds, bool force) {
        UiTreeState &tree = *entry.state;
        auto inner = tree.inner.lock();
        inner->since_build += delta_seconds;
        inner->since_rate_tick += delta_seconds;

        if (extent.x < 1.0f || extent.y < 1.0f) {
            return false;
        }
        const bool extent_changed = inner->snapshot_extent != extent;
        const bool dirty = tree.dirty.load(std::memory_order_acquire);
        bool due = force || extent_changed || !inner->snapshot;
        switch (inner->desc.rebuild) {
            case UiRebuildPolicy::EveryUpdate: due = true; break;
            case UiRebuildPolicy::OnDirty: due = due || dirty; break;
            case UiRebuildPolicy::FixedRate: {
                const f64 period = inner->desc.rebuild_hz > 0.0f ? 1.0 / static_cast<f64>(inner->desc.rebuild_hz) : 0.0;
                due = due || dirty || inner->since_rate_tick >= period;
                break;
            }
        }
        if (!due) {
            ++inner->stats.skipped;
            return false;
        }
        if (!inner->desc.builder) {
            return false;
        }
        if (t_build_depth > 0) {
            SFT_LOG_WARN_THROTTLED(5000, "UI tree '{}': not laid out from inside another tree's build (layouts on one thread cannot nest)",
                                   inner->desc.name);
            return false;
        }

        // GPU objects for trees that are drawn, when there is a device; the layout context alone otherwise (it is all a headless tree
        // or a test needs). A renderer that failed to come up is not retried every update (`ensure_ready` remembers), and the tree still
        // lays out: its overlay draws nothing.
        const bool drawn = !std::holds_alternative<UiHeadlessTarget>(inner->desc.target);
        RHI::RhiDevice *device = engine.rhi_device();
        if (drawn && device != nullptr) {
            RHI::Format format = initial_color_format(engine);
            if (const auto *offscreen = std::get_if<UiOffscreenTarget>(&inner->desc.target)) {
                if (const auto description = engine.offscreen_render_target_description(offscreen->target)) {
                    // A 10-bit target is drawn as half-float and encoded afterwards, so that is what the UI draws into.
                    format = description->format == RenderTargetFormat::Rgba16Float || description->format == RenderTargetFormat::Rgb10A2
                                 ? RHI::Format::RGBA16Float
                             : description->format == RenderTargetFormat::Rgba8Srgb ? RHI::Format::RGBA8UnormSrgb
                                                                                     : RHI::Format::BGRA8UnormSrgb;
                }
            }
            (void)inner->surface.ensure_ready(*device, format);
        }
        if (!inner->surface.ensure_context()) {
            return false;
        }

        if (const auto *window_target = std::get_if<UiWindowTarget>(&inner->desc.target)) {
            const std::optional<WindowManager::WindowId> window = resolve_window(engine, *window_target);
            const WindowSnapshot *snapshot = window ? engine.window_state().find(*window) : nullptr;
            inner->surface.set_content_scale(snapshot != nullptr ? snapshot->content_scale : 1.0f);
        } else {
            inner->surface.set_content_scale(inner->desc.content_scale);
        }
        inner->surface.set_scale_mode(inner->desc.scale_mode);

        tree.dirty.store(false, std::memory_order_release);
        const auto started = std::chrono::steady_clock::now();
        ++t_build_depth;
        UI::Context &context = inner->surface.begin_frame(extent, static_cast<f32>(inner->since_build));
        tree.pointer_consumed.store(inner->surface.input().pointer_consumed(), std::memory_order_release);
        UiTreeFrame frame{
            .ui = context,
            .engine = engine,
            .entity = entry.entity,
            .extent = extent,
            .delta_seconds = static_cast<f32>(inner->since_build),
            .build_index = inner->stats.builds,
        };
        bool failed = false;
        try {
            inner->desc.builder(frame);
        } catch (const std::exception &error) {
            failed = true;
            inner->stats.last_error = error.what();
        } catch (...) {
            failed = true;
            inner->stats.last_error = "unknown exception";
        }
        // Element scopes the builder left open were closed as the exception unwound; finishing the layout either way keeps the context
        // balanced for the next frame. A failed frame is thrown away and the previous one stays on screen.
        std::shared_ptr<UI::FrameSnapshot> snapshot;
        try {
            snapshot = std::make_shared<UI::FrameSnapshot>(context.finish_frame());
        } catch (const std::exception &error) {
            if (!failed) {
                inner->stats.last_error = error.what();
            }
            failed = true;
        }
        --t_build_depth;
        inner->surface.input().end_text_frame(); // the widgets have read what was typed, whether or not the frame is kept

        const f64 elapsed_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
        inner->since_build = 0.0;
        inner->since_rate_tick = 0.0;
        inner->stats.last_build_ms = elapsed_ms;
        inner->stats.max_build_ms = std::max(inner->stats.max_build_ms, elapsed_ms);
        if (elapsed_ms > static_cast<f64>(inner->desc.build_budget_ms)) {
            ++inner->stats.over_budget;
            SFT_LOG_WARN_THROTTLED(5000, "UI tree '{}' took {:.2f} ms to lay out (budget {:.2f} ms)", inner->desc.name, elapsed_ms,
                                   static_cast<f64>(inner->desc.build_budget_ms));
        }
        if (failed) {
            ++inner->stats.failed_builds;
            SFT_LOG_WARN_THROTTLED(5000, "UI tree '{}': build failed, keeping its previous frame: {}", inner->desc.name,
                                   inner->stats.last_error);
            return false;
        }
        if (frame.rebuild_requested) {
            tree.dirty.store(true, std::memory_order_release);
        }
        ++inner->stats.builds;
        inner->snapshot = std::move(snapshot);
        inner->snapshot_extent = extent;
        ++inner->snapshot_serial;
        return true;
    }

    void UiTrees::build(Engine &engine, f64 delta_seconds) {
        // Collect the trees first and let the query go: builders run with no ECS lock held, so they can read the world.
        std::vector<Entry> current;
        for (auto [entity, tree] : engine.ecs_world().query<UiTree>()) {
            if (tree.valid()) {
                current.push_back(Entry{.entity = entity, .state = tree.state()});
            }
        }

        RHI::RhiDevice *device = engine.rhi_device();
        {
            auto shared = shared_.lock();
            // Trees that disappeared since the last build retire: their GPU objects outlive any frame still drawing them.
            for (const Entry &old : shared->live) {
                const bool still_live = std::ranges::any_of(current, [&](const Entry &now) { return now.state == old.state; });
                if (!still_live) {
                    shared->retiring.push_back(Retiring{.state = old.state, .updates_left = retire_after_updates});
                }
            }
            for (Retiring &retiring : shared->retiring) {
                if (retiring.updates_left > 0) {
                    --retiring.updates_left;
                }
            }
            std::erase_if(shared->retiring, [device](Retiring &retiring) {
                if (retiring.updates_left > 0) {
                    return false;
                }
                // Only the GPU renderer goes: a copied handle the app kept (to put the component back later) still has its layout
                // context and widget state, and gets a renderer again at its next build.
                if (device != nullptr) {
                    retiring.state->inner.lock()->surface.release_renderer(*device);
                }
                return true;
            });
            shared->live = current;
        }

        for (const Entry &entry : current) {
            glm::vec2 extent{0.0f};
            UiTreeTarget target;
            {
                target = entry.state->inner.lock()->desc.target;
            }
            if (const auto *window_target = std::get_if<UiWindowTarget>(&target)) {
                if (const std::optional<WindowManager::WindowId> window = resolve_window(engine, *window_target)) {
                    if (const WindowSnapshot *snapshot = engine.window_state().find(*window)) {
                        extent = glm::vec2{snapshot->framebuffer_size};
                    }
                }
            } else if (const auto *offscreen = std::get_if<UiOffscreenTarget>(&target)) {
                if (const auto description = engine.offscreen_render_target_description(offscreen->target)) {
                    extent = glm::vec2{static_cast<f32>(description->width), static_cast<f32>(description->height)};
                }
            } else if (const auto *headless = std::get_if<UiHeadlessTarget>(&target)) {
                extent = glm::vec2{headless->extent};
            }
            (void)build_one(engine, entry, extent, delta_seconds, false);
        }
    }

    void UiTrees::append_window_overlays(Engine &engine, Core::RenderSurfaceHandle surface, glm::uvec2 extent,
                                         std::vector<Renderer::OverlayPass> &overlays) {
        if (extent.x == 0 || extent.y == 0) {
            return;
        }
        std::vector<Entry> trees;
        for (const Entry &entry : snapshot_live()) {
            UiTreeTarget target = entry.state->inner.lock()->desc.target;
            if (targets_window(engine, target, surface.window_id)) {
                trees.push_back(entry);
            }
        }
        std::ranges::sort(trees, [](const Entry &a, const Entry &b) {
            const i32 la = a.state->layer.load(std::memory_order_relaxed);
            const i32 lb = b.state->layer.load(std::memory_order_relaxed);
            return la != lb ? la < lb : a.state->order < b.state->order;
        });
        const glm::vec2 frame_extent{extent};
        for (const Entry &entry : trees) {
            bool matches = false;
            {
                auto inner = entry.state->inner.lock();
                matches = inner->snapshot && inner->snapshot_extent == frame_extent;
            }
            // The window changed size since the tree was laid out (the update read the window state before the resize reached it):
            // lay it out again at the size this frame really has, rather than draw a frame the overlay would reject.
            if (!matches) {
                (void)build_one(engine, entry, frame_extent, 0.0, true);
            }
            auto inner = entry.state->inner.lock();
            if (!inner->snapshot || inner->snapshot_extent != frame_extent) {
                continue;
            }
            overlays.push_back(inner->surface.make_overlay(
                inner->snapshot, engine.renderer(),
                UI::UiOverlayOptions{.name = inner->desc.name, .hdr_reference_white_scale = inner->desc.hdr_reference_white_scale}));
        }
    }

    usize UiTrees::render_targets(Engine &engine) {
        WindowManager::Window *primary = engine.primary_window();
        if (engine.rhi_device() == nullptr || primary == nullptr) {
            return 0;
        }
        usize rendered = 0;
        for (const Entry &entry : snapshot_live()) {
            Renderer::OverlayPass overlay;
            RenderTargetHandle target{};
            glm::vec2 extent{0.0f};
            glm::vec4 clear{0.0f};
            u64 frame_index = 0;
            std::string name;
            {
                auto inner = entry.state->inner.lock();
                const auto *offscreen = std::get_if<UiOffscreenTarget>(&inner->desc.target);
                if (offscreen == nullptr || !inner->snapshot || inner->snapshot_serial == inner->rendered_serial) {
                    continue;
                }
                target = offscreen->target;
                extent = inner->snapshot_extent;
                clear = inner->desc.clear_color;
                frame_index = inner->target_frame_index++;
                name = inner->desc.name;
                overlay = inner->surface.make_overlay(
                    inner->snapshot, engine.renderer(),
                    UI::UiOverlayOptions{.name = inner->desc.name, .hdr_reference_white_scale = inner->desc.hdr_reference_white_scale});
                inner->rendered_serial = inner->snapshot_serial;
            }
            const auto description = engine.offscreen_render_target_description(target);
            if (!description || glm::vec2{static_cast<f32>(description->width), static_cast<f32>(description->height)} != extent) {
                continue; // resized since the build: the next update lays it out at the new size
            }

            RenderFrameParameters params{};
            params.render_graph = RenderGraph::overlay_only();
            params.render_graph.set_background_color(clear);
            if (!params.render_graph.set_present_target(target)) {
                SFT_LOG_WARN_THROTTLED(5000, "UI tree '{}': the overlay-only graph has no single Present pass to aim at its target", name);
                continue;
            }
            params.overlay_passes.push_back(std::move(overlay));
            if (auto label = UString::try_from_utf8(name)) {
                params.debug_label = std::move(*label);
            }
            const Core::FrameInput frame{
                .delta_seconds = 0.0,
                .frame_index = frame_index,
                .framebuffer_width = description->width,
                .framebuffer_height = description->height,
            };
            // The renderer keeps per-surface frame state even for off-screen output, so the primary window's surface is borrowed;
            // nothing is presented to it (the same arrangement as an off-screen 3D view).
            const PreparedRenderFrame prepared = engine.prepare_render_frame(Core::RenderSurfaceHandle{primary->id()}, frame, params);
            const Core::RendererResult result = engine.render(prepared);
            engine.record_frame_result(result);
            if (!result) {
                SFT_LOG_WARN_THROTTLED(5000, "UI tree '{}': rendering into its target failed: {}", name, result.error().message);
                continue;
            }
            ++entry.state->inner.lock()->stats.target_renders;
            ++rendered;
        }
        return rendered;
    }

    void UiTrees::release_gpu(RHI::RhiDevice &device) noexcept {
        auto shared = shared_.lock();
        for (const Entry &entry : shared->live) {
            entry.state->inner.lock()->surface.release_renderer(device);
        }
        for (const Retiring &retiring : shared->retiring) {
            retiring.state->inner.lock()->surface.release_renderer(device);
        }
    }

    void UiTrees::shutdown(RHI::RhiDevice *device) noexcept {
        auto shared = shared_.lock();
        if (device != nullptr) {
            for (const Entry &entry : shared->live) {
                entry.state->inner.lock()->surface.release_renderer(*device);
            }
            for (const Retiring &retiring : shared->retiring) {
                retiring.state->inner.lock()->surface.release_renderer(*device);
            }
        }
        shared->retiring.clear();
        shared->live.clear();
        shared->routing.clear();
    }

} // namespace SFT::Engine
