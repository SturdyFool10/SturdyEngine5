// Live module changes: a module that registers a system, a resource and an event channel while it starts loses all three when it is
// unloaded or reloaded, the change happens at the next update boundary (not when it is requested), dependents block an unload, a
// failed start leaves nothing behind, and the start order survives a reload.

#include <Async/Scheduler.hpp>
#include <Engine/CompositeGameLogic.hpp>
#include <Engine/ModuleLedger.hpp>

#include <CxxApi/CxxApi.hpp>
#include <CxxApi/Error.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace SFT;
using namespace SFT::CxxApi;
using SFT::Engine::CompositeGameLogic;
using SFT::Engine::GameLogic;
using SFT::Engine::GameLogicError;
using SFT::Engine::GameLogicResult;
using SFT::Engine::GameModuleOptions;
using EngineT = SFT::Engine::Engine;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    struct Counters {
        int runs = 0;       // times the system body ran
        int started = 0;    // times a module's init ran
        int shut_down = 0;  // times a module's on_shutdown ran
        int destroyed = 0;  // module objects destroyed
        int system_objects_destroyed = 0;
    };

    class CountingSystem final : public ChunkSystemCallback {
      public:
        CountingSystem(int &runs, int &destroyed) : runs_(runs), destroyed_(destroyed) {}
        ~CountingSystem() override { ++destroyed_; }
        void run(const Entity *, std::size_t, void *const *, SystemCommands &) noexcept override { ++runs_; }

      private:
        int &runs_;
        int &destroyed_;
    };

    /// A module that, when it starts, registers a system over `position`, a resource and an event channel.
    class RegisteringModule final : public GameLogic {
      public:
        RegisteringModule(EngineT &engine, std::uint32_t position, std::string tag, Counters &counters, bool fail_after_registering = false)
            : engine_(engine), position_(position), tag_(std::move(tag)), counters_(counters), fail_(fail_after_registering) {}
        ~RegisteringModule() override { ++counters_.destroyed; }

        GameLogicResult on_engine_initialized(EngineT &) override {
            ++counters_.started;
            Ecs::World &world = engine_.ecs_world();
            (void)ecs_add_chunk_system(engine_, {{position_, AccessMode::Read}}, {}, std::make_unique<CountingSystem>(counters_.runs, counters_.system_objects_destroyed));
            (void)ecs_create_resource(world, "test.reload." + tag_ + ".state", 4, {}, {}, {});
            (void)ecs_create_event_channel(world, "test.reload." + tag_ + ".events", 4, {}, {});
            if (fail_) {
                return std::unexpected(GameLogicError{UString{"refusing to start"}});
            }
            return {};
        }
        std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(EngineT &, SFT::Core::RenderSurfaceHandle, const SFT::Core::FrameInput &) override {
            return std::nullopt;
        }
        void on_shutdown(EngineT &) noexcept override { ++counters_.shut_down; }

      private:
        EngineT &engine_;
        std::uint32_t position_;
        std::string tag_;
        Counters &counters_;
        bool fail_;
    };

    /// What a binding implements: registers a resource named after `tag` when it starts.
    class TaggedCallbacks final : public GameLogicCallbacks {
      public:
        TaggedCallbacks(std::string tag, int &started, int &shut_down) : tag_(std::move(tag)), started_(started), shut_down_(shut_down) {}
        bool on_engine_initialized(EngineT &engine, std::string &) override {
            ++started_;
            (void)ecs_create_resource(engine.ecs_world(), "test.callbacks." + tag_, 4, {}, {}, {});
            return true;
        }
        bool request_render_frame(EngineT &, const FrameInfo &, FrameBuilder &) override { return false; }
        void on_shutdown(EngineT &) noexcept override { ++shut_down_; }

      private:
        std::string tag_;
        int &started_;
        int &shut_down_;
    };

    bool has_resource(EngineT &engine, const std::string &name) { return ecs_has_resource(engine.ecs_world(), ecs_resource_id(name)); }

    std::string joined(const std::vector<std::string> &names) {
        std::string out;
        for (const auto &n : names) {
            out += (out.empty() ? "" : ",") + n;
        }
        return out;
    }

} // namespace

int main() {
    EngineT engine;
    Ecs::World &world = engine.ecs_world();
    const std::uint32_t position = ecs_register_component(world, "test.reload.position", 8, 4, {}, {});
    (void)ecs_spawn(world, {position}, std::vector<std::uint8_t>(8));

    Counters a1, a2, b, c, bad;
    CompositeGameLogic composite;
    check(composite.add("a", std::make_unique<RegisteringModule>(engine, position, "a", a1)).has_value(), "add a");
    check(composite.add("b", std::make_unique<RegisteringModule>(engine, position, "b", b), {.after = {"a"}}).has_value(), "add b (after a)");

    const auto base_systems = engine.update_schedule().system_count();
    check(composite.on_engine_initialized(engine).has_value(), "startup succeeds");
    check(CompositeGameLogic::of(engine) == &composite, "the running composite can be found from its engine");
    check(engine.update_schedule().system_count() == base_systems + 2, "each module registered a system");
    check(has_resource(engine, "test.reload.a.state") && has_resource(engine, "test.reload.b.events"), "and a resource and a channel");
    check(SFT::Engine::ModuleLedger::count(world, "a") == 3 && SFT::Engine::ModuleLedger::count(world, "b") == 3, "the ledger attributes three registrations to each module");

    engine.update(0.016);
    check(a1.runs == 1 && b.runs == 1, "both systems run");

    // ---- reload: nothing happens until the update boundary ----
    check(a2.started == 0, "(setup) the replacement has not started");
    GameLogicResult reload_result = std::unexpected(GameLogicError{UString{"not called"}});
    composite.request_reload(engine, "a", std::make_unique<RegisteringModule>(engine, position, "a", a2),
                             [&](GameLogicResult r) { reload_result = std::move(r); });
    check(a1.shut_down == 0 && a2.started == 0 && has_resource(engine, "test.reload.a.state"), "a requested reload does nothing until the next update");

    engine.update(0.016);
    check(reload_result.has_value(), "the reload completed at the update boundary");
    check(a1.shut_down == 1 && a1.destroyed == 1, "the old module was shut down and destroyed");
    check(a1.system_objects_destroyed == 1, "its system's callback was destroyed with it");
    check(a2.started == 1, "the replacement started");
    check(engine.update_schedule().system_count() == base_systems + 2, "the system count is back to one per module, not three");
    check(has_resource(engine, "test.reload.a.state"), "the replacement recreated the resource (the old one was destroyed first)");
    check(a2.runs == 1 && a1.runs == 1, "the replacement's system ran in the same update; the old one did not run again");
    check(joined(composite.start_order()) == "a,b", "the start order survived the reload");

    // ---- a dependent blocks an unload ----
    GameLogicResult unload_a = {};
    composite.request_unload(engine, "a", [&](GameLogicResult r) { unload_a = std::move(r); });
    engine.update(0.016);
    check(!unload_a && std::string{std::string_view{unload_a.error().message}}.find("'b' starts after it") != std::string::npos,
          "unloading a module another module starts after is refused, naming the dependent");
    check(a2.shut_down == 0, "a refused unload shuts nothing down");

    GameLogicResult unload_b = std::unexpected(GameLogicError{UString{"not called"}});
    composite.request_unload(engine, "b", [&](GameLogicResult r) { unload_b = std::move(r); });
    engine.update(0.016);
    check(unload_b.has_value() && b.shut_down == 1 && b.destroyed == 1, "an unload of a module nothing depends on succeeds");
    check(!has_resource(engine, "test.reload.b.state") && !has_resource(engine, "test.reload.b.events"), "its resource and channel are gone");
    check(engine.update_schedule().system_count() == base_systems + 1, "and so is its system");
    const int b_runs = b.runs;
    engine.update(0.016);
    check(b.runs == b_runs, "an unloaded module's system no longer runs");
    check(SFT::Engine::ModuleLedger::count(world, "b") == 0, "nothing is left on its ledger");

    GameLogicResult unload_missing = {};
    composite.request_unload(engine, "nope", [&](GameLogicResult r) { unload_missing = std::move(r); });
    engine.update(0.016);
    check(!unload_missing, "unloading a module that is not loaded is an error");

    // ---- load ----
    GameLogicResult load_c = std::unexpected(GameLogicError{UString{"not called"}});
    composite.request_load(engine, "c", std::make_unique<RegisteringModule>(engine, position, "c", c), {.after = {"a"}},
                           [&](GameLogicResult r) { load_c = std::move(r); });
    engine.update(0.016);
    check(load_c.has_value() && c.started == 1 && joined(composite.start_order()) == "a,c", "a module can be loaded into the running game, after its dependency");
    check(c.runs >= 1, "and its system runs");

    GameLogicResult load_orphan = {};
    composite.request_load(engine, "d", std::make_unique<RegisteringModule>(engine, position, "d", bad), {.after = {"ghost"}},
                           [&](GameLogicResult r) { load_orphan = std::move(r); });
    engine.update(0.016);
    check(!load_orphan && bad.started == 0, "a module whose dependency is not loaded is refused before it starts");

    GameLogicResult load_dup = {};
    composite.request_load(engine, "a", std::make_unique<RegisteringModule>(engine, position, "a2", bad), {},
                           [&](GameLogicResult r) { load_dup = std::move(r); });
    engine.update(0.016);
    check(!load_dup, "loading a name that is already loaded is refused");

    // ---- a failed start leaves nothing behind ----
    const auto systems_before = engine.update_schedule().system_count();
    GameLogicResult load_bad = {};
    composite.request_load(engine, "e", std::make_unique<RegisteringModule>(engine, position, "e", bad, true), {},
                           [&](GameLogicResult r) { load_bad = std::move(r); });
    engine.update(0.016);
    check(!load_bad && std::string{std::string_view{load_bad.error().message}}.find("refusing to start") != std::string::npos,
          "a module that fails to start reports why");
    check(engine.update_schedule().system_count() == systems_before, "the system it registered before failing was removed");
    check(!has_resource(engine, "test.reload.e.state") && !has_resource(engine, "test.reload.e.events"), "so were its resource and channel");
    check(joined(composite.start_order()) == "a,c", "and it is not in the module list");

    // ---- reloading with a replacement that fails leaves the module unloaded ----
    GameLogicResult reload_bad = {};
    composite.request_reload(engine, "c", std::make_unique<RegisteringModule>(engine, position, "c", bad, true), [&](GameLogicResult r) { reload_bad = std::move(r); });
    engine.update(0.016);
    check(!reload_bad && joined(composite.start_order()) == "a", "a replacement that fails to start leaves the module unloaded (documented)");

    // ---- shutdown ----
    composite.on_shutdown(engine);
    check(CompositeGameLogic::of(engine) == nullptr, "a shut-down composite is no longer found");
    check(a2.shut_down == 1, "shutdown ran the remaining module's on_shutdown");
    check(!has_resource(engine, "test.reload.a.state"), "and removed what it registered");

    // ---- the CxxApi surface a binding calls ----
    {
        EngineT other;
        int started_one = 0, shut_one = 0, started_two = 0, shut_two = 0;
        auto modules = runtime_modules_new();
        runtime_modules_add(*modules, "x", {}, true, std::make_unique<TaggedCallbacks>("one", started_one, shut_one));

        bool threw = false;
        try {
            module_request_unload(other, "x");
        } catch (const Error &) {
            threw = true;
        }
        check(threw, "a live change with no game running on the engine throws");

        check(modules->composite.on_engine_initialized(other).has_value() && started_one == 1, "the callbacks module starts");
        check(module_names(other)->size() == 1 && (*module_names(other))[0] == "x", "module_names lists the loaded modules");

        module_request_reload(other, "x", std::make_unique<TaggedCallbacks>("two", started_two, shut_two));
        check(started_two == 0, "a reload through the binding API waits for the update boundary");
        other.update(0.016);
        check(shut_one == 1 && started_two == 1, "then replaces the module");
        check(!ecs_has_resource(other.ecs_world(), ecs_resource_id("test.callbacks.one")) &&
                  ecs_has_resource(other.ecs_world(), ecs_resource_id("test.callbacks.two")),
              "the old module's resource is gone and the new module's exists");

        module_request_unload(other, "nope");
        module_request_load(other, "y", {"ghost"}, true, std::make_unique<TaggedCallbacks>("y", started_two, shut_two));
        other.update(0.016);
        const auto failed = module_take_failures(other);
        check(failed->size() == 2, "both refused changes were reported");
        check(failed->size() == 2 && (*failed)[0].find("nope") != std::string::npos && (*failed)[1].find("ghost") != std::string::npos,
              "with the module name or missing dependency in the message");
        check(module_take_failures(other)->empty(), "taking the failures clears them");

        module_request_unload(other, "x");
        other.update(0.016);
        check(module_names(other)->empty() && shut_two == 1, "unloading removes the module");
        modules->composite.on_shutdown(other);
        threw = false;
        try {
            module_request_unload(other, "x");
        } catch (const Error &) {
            threw = true;
        }
        check(threw, "after shutdown a live change throws again");
    }

    Async::Scheduler::shutdown();
    if (failures != 0) {
        std::fprintf(stderr, "CxxApiModuleReloadTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CxxApiModuleReloadTest: ok\n");
    return 0;
}
