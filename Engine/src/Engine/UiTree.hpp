#pragma once

#include <Foundation/Foundation.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <Async/Mutex.hpp>
#include <Core/Core.hpp>
#include <Ecs/Component.hpp>
#include <Ecs/Entity.hpp>
#include <Ecs/System.hpp>
#include <Engine/RenderTarget.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Overlay.hpp>
#include <Renderer/UI/UiSurface.hpp>
#include <WindowManager/WindowManager.hpp>

namespace SFT::Engine {

    class Engine;

    /// A UI tree is an entity with a `UiTree` component. The engine lays out every tree once per `Engine::update`, after the update
    /// schedule has run, on the thread that calls `update`; there is no UI thread to own, join or get stuck. Each tree is independent:
    /// its own layout context, widget state, input and output. A HUD, a docked editor and any number of in-world monitors are just
    /// several entities.
    ///
    /// ```cpp
    /// // A HUD on the primary window.
    /// world.spawn(Engine::UiTree{{.name = "hud", .builder = [](Engine::UiTreeFrame &f) { ... f.ui ... }}});
    /// // A monitor in the world: the tree renders into an off-screen target whose texture a material samples.
    /// world.spawn(Engine::UiTree{{.name = "monitor", .target = Engine::UiOffscreenTarget{handle},
    ///                             .rebuild = Engine::UiRebuildPolicy::OnDirty, .builder = draw_monitor}});
    /// ```
    ///
    /// **Where frames go** (`UiTreeTarget`): a window tree is attached to that window's frames as an overlay automatically (by
    /// `Engine::prepare_render_frame`; nothing to append by hand). An off-screen tree is rendered into its target by
    /// `Engine::render_ui_tree_targets`, which the application loop calls after `update`, only when the tree produced a new frame. A
    /// headless tree is only laid out (tests, measuring).
    ///
    /// **Nothing waits on a tree.** The renderer always draws a tree's latest *finished* frame. A tree that is not rebuilt this update
    /// (its `UiRebuildPolicy` said no, or its builder threw) keeps showing its last frame; it never blanks and never blocks a window.
    /// A builder that takes longer than its budget is reported in `UiTreeStats` and the log. A builder that never returns still stalls
    /// the update like any other system would; see `plans/ui-ecs-trees.md` for the asynchronous builds planned against that.
    ///
    /// **Input**: window trees are fed from that window's events by the engine (see `UiTreeDesc::window_input`): the pointer goes to the
    /// topmost tree (highest `layer`) that the pointer was over at its last build, a press captures the pointer for its tree until the
    /// release, and keyboard/text input goes to the tree that last took a press. Off-screen trees are fed by the application (a ray against
    /// the panel, a gamepad cursor) through `UiTree::with_input` / `UiTree::point_at`.
    ///
    /// **Threading**: builders run one at a time on the `update` thread, after every update system has finished, so a builder may read
    /// the ECS world (`World::get_component`, queries) and engine state freely. It must not spawn or destroy entities while it runs (queue
    /// that with `Engine::defer_to_update_boundary`) and must not call back into its own `UiTree` (use the `UiTreeFrame` it is given).

    /// The tree is drawn over a window's frames. `window` empty = the primary window.
    struct UiWindowTarget {
        std::optional<WindowManager::WindowId> window{};
    };

    /// The tree is rendered into an off-screen target (`Engine::create_offscreen_render_target`), at the target's size. Sample
    /// `Engine::offscreen_render_target_texture` on a mesh to show it in the world.
    struct UiOffscreenTarget {
        RenderTargetHandle target{};
    };

    /// The tree is laid out at `extent` pixels and drawn nowhere.
    struct UiHeadlessTarget {
        glm::uvec2 extent{0u};
    };

    using UiTreeTarget = std::variant<UiWindowTarget, UiOffscreenTarget, UiHeadlessTarget>;

    /// When a tree is laid out again. Whatever the policy, a tree is rebuilt when its extent changes or after `UiTree::request_rebuild`.
    enum class UiRebuildPolicy : u8 {
        /// Every update. Right for a HUD that shows live values.
        EveryUpdate,
        /// Only when input reached the tree, `request_rebuild` was called (by the app, or by the builder for an animation in progress),
        /// or the extent changed. Right for in-world panels and mostly static tools.
        OnDirty,
        /// At most `UiTreeDesc::rebuild_hz` times per second, plus the `OnDirty` triggers. Right for a monitor showing a clock or a graph.
        FixedRate,
    };

    /// What a builder is handed each time its tree is laid out.
    struct UiTreeFrame {
        /// The tree's layout context, between `begin_layout` and `finish_frame`. Build widgets here.
        UI::Context &ui;
        Engine &engine;
        /// The entity that carries the `UiTree`.
        Ecs::Entity entity{};
        /// Framebuffer pixels the tree is laid out at. Layout units are `extent / ui.pixel_scale()`.
        glm::vec2 extent{0.0f};
        /// Seconds since this tree was last laid out (not since the last update: a tree that was skipped sees the whole gap).
        f32 delta_seconds = 0.0f;
        /// How many times this tree has been laid out before.
        u64 build_index = 0;

        /// Lays the tree out again next update even under `OnDirty` / `FixedRate` (an animation that is still running).
        void request_rebuild() noexcept { rebuild_requested = true; }
        bool rebuild_requested = false;
    };

    using UiTreeBuilder = std::function<void(UiTreeFrame &)>;

    struct UiTreeDesc {
        /// For logs, stats and the overlay pass name.
        std::string name = "ui";
        UiTreeTarget target = UiWindowTarget{};
        UiTreeBuilder builder{};
        UiRebuildPolicy rebuild = UiRebuildPolicy::EveryUpdate;
        /// `FixedRate` only.
        f32 rebuild_hz = 30.0f;
        /// Stacking among trees on the same window: higher draws on top and gets the pointer first. Equal layers stack in spawn order.
        i32 layer = 0;
        UI::UiScaleMode scale_mode = UI::UiScaleMode::ContentScale;
        /// Content scale for off-screen and headless trees (window trees follow their window's). 1 = a layout unit is a pixel.
        f32 content_scale = 1.0f;
        /// HDR displays only: scales the reference white the UI is composed at.
        f32 hdr_reference_white_scale = 1.0f;
        /// Window trees only: whether the engine feeds this tree from its window's mouse, keyboard and text events. Turn off to feed
        /// it yourself (`UiTree::with_input`).
        bool window_input = true;
        /// Off-screen trees only: what the target is cleared to before the UI is drawn (straight-alpha linear RGBA).
        glm::vec4 clear_color{0.0f, 0.0f, 0.0f, 1.0f};
        /// A build that takes longer than this is counted in `UiTreeStats::over_budget` and logged (throttled).
        f32 build_budget_ms = 4.0f;
    };

    struct UiTreeStats {
        u64 builds = 0;
        /// Updates in which the tree was not rebuilt because its policy said it was clean.
        u64 skipped = 0;
        u64 over_budget = 0;
        u64 failed_builds = 0;
        /// Off-screen trees: frames rendered into the target.
        u64 target_renders = 0;
        f64 last_build_ms = 0.0;
        f64 max_build_ms = 0.0;
        /// Message of the last failed build ("" if none).
        std::string last_error;
    };

    struct UiTreeState;

    /// The ECS component that makes an entity a UI tree. A cheap, copyable handle: copies refer to the same tree. Destroying the
    /// entity (or removing the component) ends the tree; its GPU objects are released a few updates later, once no frame in flight can
    /// still be drawing it.
    class UiTree {
      public:
        /// An empty handle; it is ignored by the engine.
        UiTree() noexcept = default;
        explicit UiTree(UiTreeDesc desc);

        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

        void set_builder(UiTreeBuilder builder);
        void set_target(UiTreeTarget target);
        [[nodiscard]] UiTreeTarget target() const;
        void set_rebuild_policy(UiRebuildPolicy policy, f32 rebuild_hz = 30.0f);
        void set_layer(i32 layer);
        [[nodiscard]] i32 layer() const;

        /// Lays the tree out at the next update whatever its policy. Any thread.
        void request_rebuild() noexcept;

        /// Runs `fn` on the tree's input under the tree's lock, and marks the tree dirty. The way to drive an off-screen or headless
        /// tree (or a window tree with `window_input` off). Do not call from inside this tree's builder.
        void with_input(const std::function<void(UI::UiInput &)> &fn);

        /// Points at the tree: `uv` in [0, 1] across it (`UI::ui_uv_at_ray` gives one for a world panel), or empty when the pointer
        /// is not on it (cancels a press in progress). `down` is the primary button.
        void point_at(std::optional<glm::vec2> uv, bool down);

        /// Whether the pointer was over the tree (or dragging in it) at its last build: the "did the UI take this click" answer.
        [[nodiscard]] bool pointer_consumed() const;
        [[nodiscard]] UiTreeStats stats() const;
        /// Extent of the tree's latest finished frame (zero before the first).
        [[nodiscard]] glm::vec2 last_extent() const;

        [[nodiscard]] const std::shared_ptr<UiTreeState> &state() const noexcept { return state_; }

      private:
        std::shared_ptr<UiTreeState> state_;
    };

    /// The engine's side of the UI trees: lays them out, routes window input to them, attaches and renders their frames. Owned by
    /// `Engine`; applications use `UiTree` and the `Engine` entry points (`ui_trees()`), not this directly.
    class UiTrees {
      public:
        UiTrees();
        ~UiTrees();
        UiTrees(const UiTrees &) = delete;
        UiTrees &operator=(const UiTrees &) = delete;

        /// Adds the window-input routing system to `schedule`. Called once by the engine.
        void install(Engine &engine, Ecs::Schedule &schedule);

        /// Lays out every tree that is due. Called by `Engine::update` after the update schedule.
        void build(Engine &engine, f64 delta_seconds);

        /// Appends the overlays of the trees drawn on `surface`'s window, lowest layer first. A tree whose last frame was laid out at
        /// another extent (the window was resized since) is laid out again at `extent` first, so a resize never shows a stretched or
        /// mismatched UI.
        void append_window_overlays(Engine &engine, Core::RenderSurfaceHandle surface, glm::uvec2 extent,
                                    std::vector<Renderer::OverlayPass> &overlays);

        /// Renders every off-screen tree that has a frame its target has not shown yet. Returns how many were rendered.
        usize render_targets(Engine &engine);

        /// Drops every tree's GPU renderer (device switch); layout contexts are kept and renderers come back on the next build.
        void release_gpu(RHI::RhiDevice &device) noexcept;
        /// Releases everything that touches the device. Called by the engine before the device goes away.
        void shutdown(RHI::RhiDevice *device) noexcept;

        /// Trees seen at the last build.
        [[nodiscard]] usize tree_count() const;

      private:
        struct Entry {
            Ecs::Entity entity{};
            std::shared_ptr<UiTreeState> state;
        };
        struct Retiring {
            std::shared_ptr<UiTreeState> state;
            u32 updates_left = 0;
        };
        struct WindowRouting {
            std::weak_ptr<UiTreeState> capture;
            std::weak_ptr<UiTreeState> focus;
        };
        struct Shared {
            std::vector<Entry> live;
            std::vector<Retiring> retiring;
            std::unordered_map<WindowManager::WindowId, WindowRouting> routing;
        };

        [[nodiscard]] std::vector<Entry> snapshot_live() const;
        /// Window trees that take `window`'s events, topmost first.
        [[nodiscard]] static std::vector<std::shared_ptr<UiTreeState>> input_trees_for(Engine &engine, const std::vector<Entry> &live,
                                                                                      WindowManager::WindowId window);
        /// Lays one tree out at `extent` if `force` or its policy says so. Caller holds no tree lock.
        bool build_one(Engine &engine, const Entry &entry, glm::vec2 extent, f64 delta_seconds, bool force);

        mutable Async::Mutex<Shared> shared_;
        Ecs::SystemHandle router_system_{};
    };

} // namespace SFT::Engine

SFT_ECS_COMPONENT(SFT::Engine::UiTree, "sturdy.engine.ui_tree");
