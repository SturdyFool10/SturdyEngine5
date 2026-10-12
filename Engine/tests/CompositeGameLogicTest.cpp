// CompositeGameLogic: start order (registration order + `after`), startup failure unwinding, shutdown order, and how frames
// from several modules are merged. Pure CPU: the engine is never initialized, modules are recording fakes.

#include <Engine/CompositeGameLogic.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace SFT;
using SFT::Engine::CompositeGameLogic;
using SFT::Engine::GameLogic;
using SFT::Engine::GameLogicError;
using SFT::Engine::GameLogicResult;
using SFT::Engine::RenderFrameParameters;
using EngineT = SFT::Engine::Engine;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    /// Appends "<tag>+" / "<tag>-" to a shared log on init / shutdown.
    class Recorder final : public GameLogic {
      public:
        Recorder(std::string tag, std::vector<std::string> &log) : tag_(std::move(tag)), log_(log) {}

        bool fail_init = false;
        bool returns_frame = true;
        float exposure = 1.0f;
        std::vector<std::string> overlays;

        [[nodiscard]] GameLogicResult on_engine_initialized(EngineT &) override {
            if (fail_init) {
                return std::unexpected(GameLogicError{UString{"boom"}});
            }
            log_.push_back(tag_ + "+");
            return {};
        }

        [[nodiscard]] std::optional<RenderFrameParameters> request_render_frame(EngineT &, Core::RenderSurfaceHandle,
                                                                                const Core::FrameInput &) override {
            if (!returns_frame) {
                return std::nullopt;
            }
            RenderFrameParameters frame;
            frame.lighting.exposure = exposure;
            frame.debug_label = UString{std::string_view{tag_}};
            for (const std::string &name : overlays) {
                frame.overlay_passes.push_back(Renderer::OverlayPass{.name = name});
            }
            return frame;
        }

        void on_shutdown(EngineT &) noexcept override { log_.push_back(tag_ + "-"); }

      private:
        std::string tag_;
        std::vector<std::string> &log_;
    };

    std::string joined(const std::vector<std::string> &v) {
        std::string out;
        for (const auto &s : v) {
            out += (out.empty() ? "" : " ") + s;
        }
        return out;
    }

} // namespace

int main() {
    EngineT engine;
    const Core::FrameInput frame{};
    const Core::RenderSurfaceHandle surface{};
    std::vector<std::string> log;

    // ---- registration validation ----
    {
        CompositeGameLogic composite;
        check(!composite.add("", std::make_unique<Recorder>("x", log)), "empty name is rejected");
        check(!composite.add("x", nullptr), "null module is rejected");
        check(composite.add("a", std::make_unique<Recorder>("a", log)).has_value(), "first module is accepted");
        check(!composite.add("a", std::make_unique<Recorder>("a", log)), "duplicate name is rejected");
        check(!composite.add("b", std::make_unique<Recorder>("b", log), {.after = {"b"}}), "a module cannot follow itself");
        check(composite.module_count() == 1, "rejected modules are not added");
    }

    // ---- start order: registration order, then `after` ----
    {
        log.clear();
        CompositeGameLogic composite;
        check(composite.add("gameplay", std::make_unique<Recorder>("gameplay", log), {.after = {"physics", "input"}}).has_value(), "add gameplay");
        check(composite.add("hud", std::make_unique<Recorder>("hud", log), {.after = {"gameplay"}}).has_value(), "add hud");
        check(composite.add("physics", std::make_unique<Recorder>("physics", log)).has_value(), "add physics");
        check(composite.add("input", std::make_unique<Recorder>("input", log)).has_value(), "add input");
        check(composite.on_engine_initialized(engine).has_value(), "startup succeeds");
        check(joined(composite.start_order()) == "physics input gameplay hud", "dependencies start first, ties keep registration order");
        check(joined(log) == "physics+ input+ gameplay+ hud+", "modules initialize in start order");
        log.clear();
        composite.on_shutdown(engine);
        check(joined(log) == "hud- gameplay- input- physics-", "shutdown is the reverse of start order");
        log.clear();
        composite.on_shutdown(engine);
        check(log.empty(), "a second shutdown does nothing");
        check(!composite.add("late", std::make_unique<Recorder>("late", log)), "adding after startup is rejected");
    }

    // ---- bad dependency graphs ----
    {
        CompositeGameLogic unknown;
        check(unknown.add("a", std::make_unique<Recorder>("a", log), {.after = {"ghost"}}).has_value(), "add a");
        const auto result = unknown.on_engine_initialized(engine);
        check(!result && std::string_view{result.error().message}.contains("ghost"), "unknown dependency is named in the error");

        CompositeGameLogic cycle;
        check(cycle.add("a", std::make_unique<Recorder>("a", log), {.after = {"b"}}).has_value(), "cycle a");
        check(cycle.add("b", std::make_unique<Recorder>("b", log), {.after = {"a"}}).has_value(), "cycle b");
        const auto cycled = cycle.on_engine_initialized(engine);
        check(!cycled && std::string_view{cycled.error().message}.contains("cycle"), "a dependency cycle is reported");
        check(joined(cycle.start_order()) == "a b", "a cycle leaves the modules untouched");

        CompositeGameLogic empty;
        check(!empty.on_engine_initialized(engine), "a game with no modules fails to start");
    }

    // ---- startup failure unwinds what already started ----
    {
        log.clear();
        CompositeGameLogic composite;
        auto bad = std::make_unique<Recorder>("bad", log);
        bad->fail_init = true;
        check(composite.add("first", std::make_unique<Recorder>("first", log)).has_value(), "add first");
        check(composite.add("second", std::make_unique<Recorder>("second", log)).has_value(), "add second");
        check(composite.add("bad", std::move(bad)).has_value(), "add bad");
        check(composite.add("never", std::make_unique<Recorder>("never", log)).has_value(), "add never");
        const auto result = composite.on_engine_initialized(engine);
        check(!result, "a failing module fails startup");
        check(result.error().message == UString{"module 'bad' failed to initialize: boom"}, "the error names the module and its reason");
        check(joined(log) == "first+ second+ second- first-", "started modules shut down in reverse; later ones never start");
        check(!composite.request_render_frame(engine, surface, frame).has_value(), "a failed game renders nothing");
    }

    // ---- frame merging ----
    {
        log.clear();
        CompositeGameLogic composite;
        auto world = std::make_unique<Recorder>("world", log);
        world->exposure = 2.0f;
        world->overlays = {"world-debug"};
        auto look = std::make_unique<Recorder>("look", log);
        look->exposure = 3.0f;
        look->overlays = {"look-vignette"};
        auto hud = std::make_unique<Recorder>("hud", log);
        hud->exposure = 99.0f;
        hud->overlays = {"hud-crosshair", "hud-ammo"};
        auto quiet = std::make_unique<Recorder>("quiet", log);
        quiet->returns_frame = false;
        check(composite.add("world", std::move(world)).has_value(), "add world");
        check(composite.add("look", std::move(look), {.after = {"world"}}).has_value(), "add look");
        check(composite.add("quiet", std::move(quiet)).has_value(), "add quiet");
        check(composite.add("hud", std::move(hud), {.after = {"look"}, .contributes_scene = false}).has_value(), "add hud");
        check(composite.on_engine_initialized(engine).has_value(), "startup succeeds");

        const auto merged = composite.request_render_frame(engine, surface, frame);
        check(merged.has_value(), "a scene module returned a frame");
        if (merged) {
            check(merged->lighting.exposure == 3.0f, "the later scene module wins; the overlay-only one never replaces it");
            check(merged->debug_label == UString{"look"}, "scene fields come from the winning module");
            std::vector<std::string> names;
            for (const auto &overlay : merged->overlay_passes) {
                names.push_back(overlay.name);
            }
            check(joined(names) == "world-debug look-vignette hud-crosshair hud-ammo", "every module's overlays are kept in start order");
        }
    }

    // ---- no scene module means no frame ----
    {
        CompositeGameLogic composite;
        auto hud = std::make_unique<Recorder>("hud", log);
        hud->overlays = {"hud"};
        check(composite.add("hud", std::move(hud), {.contributes_scene = false}).has_value(), "add overlay-only module");
        check(composite.on_engine_initialized(engine).has_value(), "startup succeeds");
        check(!composite.request_render_frame(engine, surface, frame).has_value(), "overlays alone do not make a frame");
    }

    if (failures != 0) {
        std::fprintf(stderr, "CompositeGameLogicTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CompositeGameLogicTest: ok\n");
    return 0;
}
