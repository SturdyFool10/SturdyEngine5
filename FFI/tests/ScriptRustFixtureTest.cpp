/// Loads the `script_fixture` Rust library (Rust/examples/script_fixture: a self-test of the `sturdy-script` crate) and runs it against a real
/// headless engine with a running composite game. The library leaves the C ABI undefined, like a plugin, so this executable must export it.

#include <cstdio>
#include <memory>
#include <string>

#include <dlfcn.h>

#include <Async/Scheduler.hpp>
#include <Engine/CompositeGameLogic.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>
#include <FFI/Sturdy.h>

namespace {

    class Host final : public SFT::Engine::GameLogic {
      public:
        SFT::Engine::GameLogicResult on_engine_initialized(SFT::Engine::Engine &) override { return {}; }
        std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(SFT::Engine::Engine &, SFT::Core::RenderSurfaceHandle, const SFT::Core::FrameInput &) override {
            return std::nullopt;
        }
    };

    using Step = int (*)(SturdyEngine, char *, size_t);

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)std::fprintf(stderr, "usage: ScriptRustFixtureTest <path to libscript_fixture>\n");
        return 2;
    }
    // Referenced so a static build links the script ABI unit (and the others the fixture needs) into this executable.
    static const void *volatile anchors[] = {reinterpret_cast<const void *>(&sturdy_script_language_register), reinterpret_cast<const void *>(&sturdy_module_request_load),
                                             reinterpret_cast<const void *>(&sturdy_op_status), reinterpret_cast<const void *>(&sturdy_last_error_message), reinterpret_cast<const void *>(&sturdy_bytes_sink_set)};
    for (const void *anchor : anchors) {
        if (anchor == nullptr) return 3;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        (void)std::fprintf(stderr, "ScriptRustFixtureTest: cannot load %s: %s\n", argv[1], dlerror());
        return 4;
    }
    const auto load = [&](const char *name) { return reinterpret_cast<Step>(dlsym(library, name)); };
    const Step setup = load("sturdy_script_fixture_setup"), probe = load("sturdy_script_fixture_probe"), verify = load("sturdy_script_fixture_verify"),
               finish = load("sturdy_script_fixture_finish");
    if (setup == nullptr || probe == nullptr || verify == nullptr || finish == nullptr) {
        (void)std::fprintf(stderr, "ScriptRustFixtureTest: the library lacks a fixture entry point\n");
        return 5;
    }

    SFT::Engine::Engine engine_object;
    const SFT::Ffi::ScopedHandle handle{SFT::Ffi::HandleKind::Engine, &engine_object};
    const SturdyEngine engine{handle.token()};
    SFT::Engine::CompositeGameLogic composite;
    if (!composite.add("host", std::make_unique<Host>()).has_value() || !composite.on_engine_initialized(engine_object).has_value()) return 6;

    int failed = 0;
    std::string message(4096, '\0');
    const auto run = [&](const char *what, Step step) {
        message.assign(4096, '\0');
        const int count = step(engine, message.data(), message.size());
        if (count != 0) (void)std::fprintf(stderr, "ScriptRustFixtureTest: %s: %d check(s) failed: %s\n", what, count, message.c_str());
        failed += count;
    };
    run("setup", setup);
    engine_object.update(0.016); // loads both scripts
    engine_object.update(0.016);
    engine_object.update(0.016);
    run("probe", probe);
    engine_object.update(0.016); // delivers messages, swaps the reloaded script in
    engine_object.update(0.016);
    run("verify", verify);
    engine_object.update(0.016); // unloads
    run("finish", finish);
    return failed == 0 ? 0 : 1;
}
