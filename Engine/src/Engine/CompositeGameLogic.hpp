#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <Engine/GameLogic.hpp>

namespace SFT::Engine {

    /// How a module takes part in a `CompositeGameLogic`.
    struct GameModuleOptions {
        /// Names of modules that must start before this one (and shut down after it). Unknown names are reported by
        /// `on_engine_initialized`, since a module may be added after the ones that name it.
        std::vector<std::string> after;
        /// False makes this module overlay-only (see `CompositeGameLogic`).
        bool contributes_scene = true;
    };

    /// Several game-logic modules driven as one, so code written in different languages (or by different teams) can share one
    /// process and one frame.
    ///
    /// A module is any `GameLogic`: a C++ class, or a bridge object that forwards to Rust, C#, Java or a script. Modules run in
    /// *start order*: registration order, adjusted so every module comes after the ones it names in `ModuleOptions::after`.
    /// Initialization runs in start order, shutdown in the reverse, and a module that fails to initialize shuts down the ones
    /// already started (reverse order) before the error is returned, so a half-started game never keeps running.
    ///
    /// Frames: every module is asked for a frame in start order. A module with `contributes_scene` (the default) supplies the
    /// camera, lighting, render graph, debug label and camera-history flag; when several do, the *later* one wins, so a later
    /// module can override the look an earlier one set. Every module's `overlay_passes` are kept, concatenated in start
    /// order, so a HUD module written in one language draws over a scene module written in another. A module with
    /// `contributes_scene == false` is overlay-only and never replaces the scene. If no scene module returns a frame, the
    /// frame is skipped.
    ///
    /// Threading: modules are only added before startup. After that the module list never changes, and this class calls modules
    /// without holding any lock, so `request_render_frame` is as concurrent as the engine's callers (it can be called from
    /// several per-window render threads at once). Each module is responsible for its own state.
    class CompositeGameLogic final : public GameLogic {
      public:
        /// Adds a module.
        ///
        /// @param name Unique, non-empty name; dependencies refer to it and errors quote it.
        /// @param logic The module; the composite owns it.
        /// @param options Ordering and frame role.
        ///
        /// @return An error for an empty or duplicate name, a null module, a module that names itself in `after`, or any call
        ///         after startup; the module is not added then.
        [[nodiscard]] GameLogicResult add(std::string name, std::unique_ptr<GameLogic> logic, GameModuleOptions options = {});

        /// The number of modules added.
        [[nodiscard]] std::size_t module_count() const {
            const std::shared_lock lock{mutex_};
            return modules_.size();
        }

        /// Module names in start order. Registration order until startup resolves the dependencies.
        [[nodiscard]] std::vector<std::string> start_order() const;

        /// Resolves the start order, then initializes every module in it. Fails (after shutting down the modules already
        /// started) on an unknown or cyclic dependency, or when a module's own initialization fails; the error names the module.
        [[nodiscard]] GameLogicResult on_engine_initialized(Engine &engine) override;

        [[nodiscard]] std::optional<RenderFrameParameters> request_render_frame(Engine &engine, Core::RenderSurfaceHandle surface,
                                                                                const Core::FrameInput &frame) override;

        /// Shuts down the started modules in reverse start order, undoing what each registered. Safe to call more than once.
        void on_shutdown(Engine &engine) noexcept override;

        // ---- changing the module list while the game runs ----
        //
        // Each request runs at the start of the next `Engine::update` (`Engine::defer_to_update_boundary`), the one moment nothing is
        // running, so a module can be removed without a system or frame callback still using it. They may be called from any thread,
        // including from inside the module being replaced. `done` (optional) is called after the operation, on the thread that runs
        // `update`, with its outcome. Frames wait for an operation in progress, so a module never sees a half-changed list.
        //
        // What a module registered while starting (systems and resources or event channels created through the bindings, anything
        // noted in `ModuleLedger`) goes away with it. Components cannot be unregistered and stay.

        /// The composite running on `engine`, or null when none is (before startup, after shutdown, or a plain game logic).
        [[nodiscard]] static CompositeGameLogic *of(Engine &engine) noexcept;

        /// Unloads `name`: its `on_shutdown` runs, what it registered is removed, and it is destroyed. Refused when another module
        /// names it in `after`.
        void request_unload(Engine &engine, std::string name, std::function<void(GameLogicResult)> done = {});

        /// Adds and starts a module in a running game, after the modules it depends on. Fails when the name is taken, a dependency is
        /// missing or not started, or the module's initialization fails (what it registered by then is removed again).
        void request_load(Engine &engine, std::string name, std::unique_ptr<GameLogic> logic, GameModuleOptions options,
                          std::function<void(GameLogicResult)> done = {});

        /// Replaces `name` with `replacement` in the same place in the start order and with the same options, so frame merging and
        /// dependents keep working. The old module is shut down and its registrations removed first; if `replacement` fails to
        /// initialize the module stays unloaded and `done` carries the error.
        void request_reload(Engine &engine, std::string name, std::unique_ptr<GameLogic> replacement,
                            std::function<void(GameLogicResult)> done = {});

      private:
        struct Module {
            std::string name;
            std::unique_ptr<GameLogic> logic;
            GameModuleOptions options;
            bool started = false;
        };

        /// Reorders `modules_` into start order; the error names the module and the dependency at fault.
        [[nodiscard]] GameLogicResult resolve_order();

        /// Shuts down the started modules in reverse order and removes what each registered; the caller holds the exclusive lock.
        void shutdown_locked(Engine &engine) noexcept;
        /// Unload / load bodies; the caller holds the exclusive lock.
        [[nodiscard]] GameLogicResult unload_locked(Engine &engine, const std::string &name, bool check_dependents);
        [[nodiscard]] GameLogicResult load_locked(Engine &engine, std::string name, std::unique_ptr<GameLogic> logic, GameModuleOptions options,
                                                  std::optional<std::size_t> position);

        /// Guards `modules_` against the render threads' frame calls: frames hold it shared, live changes hold it exclusive.
        mutable std::shared_mutex mutex_;
        std::vector<Module> modules_;
        bool resolved_ = false;
        Engine *engine_ = nullptr;
    };

} // namespace SFT::Engine
