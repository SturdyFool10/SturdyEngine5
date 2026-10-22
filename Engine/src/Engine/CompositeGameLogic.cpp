#include <Engine/CompositeGameLogic.hpp>
#include <Foundation/Trace.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include <Engine/ModuleLedger.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] std::unexpected<GameLogicError> fail(const std::string &message) {
            return std::unexpected(GameLogicError{UString{std::string_view{message}}});
        }

        /// The running composite per engine, for `CompositeGameLogic::of`.
        std::mutex g_composites_mutex;
        std::map<const Engine *, CompositeGameLogic *> g_composites;

    } // namespace

    GameLogicResult CompositeGameLogic::add(std::string name, std::unique_ptr<GameLogic> logic, GameModuleOptions options) {
        const std::unique_lock lock{mutex_};
        if (resolved_) {
            return fail(std::format("cannot add module '{}': the game has already started", name));
        }
        if (name.empty()) {
            return fail("a game-logic module needs a non-empty name");
        }
        if (logic == nullptr) {
            return fail(std::format("module '{}' has no game logic", name));
        }
        if (std::ranges::any_of(modules_, [&](const Module &m) { return m.name == name; })) {
            return fail(std::format("a module named '{}' was already added", name));
        }
        if (std::ranges::find(options.after, name) != options.after.end()) {
            return fail(std::format("module '{}' cannot start after itself", name));
        }
        modules_.push_back(Module{.name = std::move(name), .logic = std::move(logic), .options = std::move(options)});
        return {};
    }

    std::vector<std::string> CompositeGameLogic::start_order() const {
        const std::shared_lock lock{mutex_};
        std::vector<std::string> names;
        names.reserve(modules_.size());
        for (const Module &module : modules_) {
            names.push_back(module.name);
        }
        return names;
    }

    GameLogicResult CompositeGameLogic::resolve_order() {
        // Kahn's algorithm with registration order as the tie-break, so modules with no constraints between them keep the
        // order they were added in.
        for (const Module &module : modules_) {
            for (const std::string &dependency : module.options.after) {
                if (std::ranges::none_of(modules_, [&](const Module &m) { return m.name == dependency; })) {
                    return fail(std::format("module '{}' must start after '{}', which was never added", module.name, dependency));
                }
            }
        }

        // Work on indices so nothing is moved until the whole order is known; a cycle then leaves the modules untouched.
        std::vector<std::size_t> order;
        order.reserve(modules_.size());
        std::vector<bool> placed(modules_.size(), false);
        const auto is_placed = [&](const std::string &name) {
            for (std::size_t i = 0; i < modules_.size(); ++i) {
                if (modules_[i].name == name) {
                    return static_cast<bool>(placed[i]);
                }
            }
            return false;
        };

        while (order.size() < modules_.size()) {
            const auto next = [&]() -> std::optional<std::size_t> {
                for (std::size_t i = 0; i < modules_.size(); ++i) {
                    if (!placed[i] && std::ranges::all_of(modules_[i].options.after, is_placed)) {
                        return i;
                    }
                }
                return std::nullopt;
            }();
            if (!next) {
                std::string stuck;
                for (std::size_t i = 0; i < modules_.size(); ++i) {
                    if (!placed[i]) {
                        stuck += (stuck.empty() ? "'" : ", '") + modules_[i].name + "'";
                    }
                }
                return fail(std::format("modules {} depend on each other in a cycle", stuck));
            }
            placed[*next] = true;
            order.push_back(*next);
        }

        std::vector<Module> ordered;
        ordered.reserve(modules_.size());
        for (const std::size_t index : order) {
            ordered.push_back(std::move(modules_[index]));
        }
        modules_ = std::move(ordered);
        return {};
    }

    GameLogicResult CompositeGameLogic::on_engine_initialized(Engine &engine) {
        const std::unique_lock lock{mutex_};
        if (modules_.empty()) {
            return fail("the game has no game-logic modules");
        }
        if (auto resolved = resolve_order(); !resolved) {
            return resolved;
        }
        resolved_ = true;
        engine_ = &engine;

        for (Module &module : modules_) {
            // Whatever the module registers while it starts is attributed to it, so unloading it can take that away again.
            GameLogicResult result;
            {
                const ModuleScope scope{module.name};
                // Module names are a small, stable set chosen by the application, so each makes a good trace scope.
                const Foundation::TraceScope trace_scope{Foundation::intern_trace_name(module.name)};
                result = module.logic->on_engine_initialized(engine);
            }
            if (!result) {
                const std::string reason{std::string_view{result.error().message}};
                // The module that failed may have registered things before it did; they are removed with the others.
                (void)ModuleLedger::release(engine.ecs_world(), module.name);
                shutdown_locked(engine);
                engine_ = nullptr;
                return fail(std::format("module '{}' failed to initialize: {}", module.name, reason));
            }
            module.started = true;
        }
        {
            const std::lock_guard<std::mutex> registry_lock{g_composites_mutex};
            g_composites[&engine] = this;
        }
        return {};
    }

    CompositeGameLogic *CompositeGameLogic::of(Engine &engine) noexcept {
        const std::lock_guard<std::mutex> lock{g_composites_mutex};
        const auto found = g_composites.find(&engine);
        return found == g_composites.end() ? nullptr : found->second;
    }

    std::optional<RenderFrameParameters> CompositeGameLogic::request_render_frame(Engine &engine, Core::RenderSurfaceHandle surface,
                                                                                  const Core::FrameInput &frame) {
        // Held shared for the whole call: a live load, unload or reload waits for frames in flight, so a module is never destroyed
        // while a render thread is inside it.
        const std::shared_lock lock{mutex_};
        std::optional<RenderFrameParameters> scene;
        std::vector<Renderer::OverlayPass> overlays;
        for (Module &module : modules_) {
            if (!module.started) {
                continue;
            }
            const Foundation::TraceScope trace_scope{Foundation::intern_trace_name(module.name)};
            std::optional<RenderFrameParameters> contribution = module.logic->request_render_frame(engine, surface, frame);
            if (!contribution) {
                continue;
            }
            for (Renderer::OverlayPass &overlay : contribution->overlay_passes) {
                overlays.push_back(std::move(overlay));
            }
            contribution->overlay_passes.clear();
            if (module.options.contributes_scene) {
                scene = std::move(contribution);
            }
        }
        if (!scene) {
            return std::nullopt;
        }
        scene->overlay_passes = std::move(overlays);
        return scene;
    }

    void CompositeGameLogic::on_shutdown(Engine &engine) noexcept {
        {
            const std::lock_guard<std::mutex> registry_lock{g_composites_mutex};
            const auto found = g_composites.find(&engine);
            if (found != g_composites.end() && found->second == this) {
                g_composites.erase(found);
            }
        }
        const std::unique_lock lock{mutex_};
        shutdown_locked(engine);
        engine_ = nullptr;
    }

    void CompositeGameLogic::shutdown_locked(Engine &engine) noexcept {
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
            if (it->started) {
                it->started = false;
                it->logic->on_shutdown(engine);
                // Its systems may call into it, so they go before it is destroyed.
                (void)ModuleLedger::release(engine.ecs_world(), it->name);
            }
        }
    }

    GameLogicResult CompositeGameLogic::unload_locked(Engine &engine, const std::string &name, bool check_dependents) {
        const auto found = std::ranges::find_if(modules_, [&](const Module &m) { return m.name == name; });
        if (found == modules_.end()) {
            return fail(std::format("no module named '{}' is loaded", name));
        }
        if (check_dependents) {
            for (const Module &other : modules_) {
                if (std::ranges::find(other.options.after, name) != other.options.after.end()) {
                    return fail(std::format("module '{}' cannot be unloaded: '{}' starts after it", name, other.name));
                }
            }
        }
        if (found->started) {
            found->started = false;
            found->logic->on_shutdown(engine);
        }
        (void)ModuleLedger::release(engine.ecs_world(), name);
        modules_.erase(found); // destroys the logic (and so runs its `destroy` hook)
        return {};
    }

    GameLogicResult CompositeGameLogic::load_locked(Engine &engine, std::string name, std::unique_ptr<GameLogic> logic,
                                                    GameModuleOptions options, std::optional<std::size_t> position) {
        if (name.empty()) {
            return fail("a game-logic module needs a non-empty name");
        }
        if (logic == nullptr) {
            return fail(std::format("module '{}' has no game logic", name));
        }
        if (std::ranges::any_of(modules_, [&](const Module &m) { return m.name == name; })) {
            return fail(std::format("a module named '{}' is already loaded", name));
        }
        for (const std::string &dependency : options.after) {
            if (dependency == name) {
                return fail(std::format("module '{}' cannot start after itself", name));
            }
            const auto needed = std::ranges::find_if(modules_, [&](const Module &m) { return m.name == dependency; });
            if (needed == modules_.end() || !needed->started) {
                return fail(std::format("module '{}' must start after '{}', which is not loaded", name, dependency));
            }
        }

        GameLogicResult started;
        {
            const ModuleScope scope{name};
            started = logic->on_engine_initialized(engine);
        }
        if (!started) {
            const std::string reason{std::string_view{started.error().message}};
            // Whatever it registered before failing is removed; it is not added. Its own shutdown does not run: it never started.
            (void)ModuleLedger::release(engine.ecs_world(), name);
            return fail(std::format("module '{}' failed to initialize: {}", name, reason));
        }
        const std::size_t index = position ? std::min(*position, modules_.size()) : modules_.size();
        modules_.insert(modules_.begin() + static_cast<std::ptrdiff_t>(index),
                        Module{.name = std::move(name), .logic = std::move(logic), .options = std::move(options), .started = true});
        return {};
    }

    void CompositeGameLogic::request_unload(Engine &engine, std::string name, std::function<void(GameLogicResult)> done) {
        engine.defer_to_update_boundary([this, &engine, name = std::move(name), done = std::move(done)] {
            GameLogicResult result;
            {
                const std::unique_lock lock{mutex_};
                result = engine_ == &engine ? unload_locked(engine, name, true) : fail("the game is not running");
            }
            if (done) {
                done(std::move(result));
            }
        });
    }

    void CompositeGameLogic::request_load(Engine &engine, std::string name, std::unique_ptr<GameLogic> logic, GameModuleOptions options,
                                          std::function<void(GameLogicResult)> done) {
        // `std::function` needs a copyable callable, so the move-only logic travels in a shared pointer.
        auto shared = std::make_shared<std::unique_ptr<GameLogic>>(std::move(logic));
        engine.defer_to_update_boundary([this, &engine, name = std::move(name), shared, options = std::move(options), done = std::move(done)]() mutable {
            GameLogicResult result;
            {
                const std::unique_lock lock{mutex_};
                result = engine_ == &engine ? load_locked(engine, std::move(name), std::move(*shared), std::move(options), std::nullopt)
                                            : fail("the game is not running");
            }
            if (done) {
                done(std::move(result));
            }
        });
    }

    void CompositeGameLogic::request_reload(Engine &engine, std::string name, std::unique_ptr<GameLogic> replacement,
                                            std::function<void(GameLogicResult)> done) {
        auto shared = std::make_shared<std::unique_ptr<GameLogic>>(std::move(replacement));
        engine.defer_to_update_boundary([this, &engine, name = std::move(name), shared, done = std::move(done)]() mutable {
            GameLogicResult result;
            {
                const std::unique_lock lock{mutex_};
                if (engine_ != &engine) {
                    result = fail("the game is not running");
                } else if (*shared == nullptr) {
                    result = fail(std::format("module '{}' has no replacement game logic", name));
                } else {
                    const auto found = std::ranges::find_if(modules_, [&](const Module &m) { return m.name == name; });
                    if (found == modules_.end()) {
                        result = fail(std::format("no module named '{}' is loaded", name));
                    } else {
                        // Remember where it sat and how it was configured, then swap: dependents stay valid because the name is
                        // back before anything else can run (nothing runs while the exclusive lock is held).
                        const std::size_t position = static_cast<std::size_t>(found - modules_.begin());
                        GameModuleOptions options = found->options;
                        result = unload_locked(engine, name, false);
                        if (result) {
                            result = load_locked(engine, name, std::move(*shared), std::move(options), position);
                        }
                    }
                }
            }
            if (done) {
                done(std::move(result));
            }
        });
    }

} // namespace SFT::Engine
