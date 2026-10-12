/// Exercises native plugins (Sturdy.h, "Native plugins") against a real shared library built from `fixtures/PluginFixture.c`, which
/// knows nothing of the engine but the C header. The fixture appends what happens to it to a log file (a closed library's memory
/// cannot be read), and this test checks that log: the library is opened and described, its modules run inside a real composite
/// next to a C++ module, a reload swaps same-named modules in place and hands state to the new generation, and the library is
/// closed exactly once, after the last module it handed out was destroyed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Async/Scheduler.hpp>
#include <Engine/CompositeGameLogic.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

#ifndef STURDY_PLUGIN_FIXTURE
#error "STURDY_PLUGIN_FIXTURE must name the fixture library"
#endif

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "PluginFfiTest: %s\n", description);
            ++failures;
        }
    }

    std::filesystem::path g_log_path;

    void set_env(const char *name, const char *value) {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    void clear_log() {
        std::error_code ignored;
        std::filesystem::remove(g_log_path, ignored);
    }

    std::vector<std::string> log_lines() {
        std::vector<std::string> lines;
        std::ifstream file{g_log_path};
        for (std::string line; std::getline(file, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    /// Index of the first line equal to `text` at or after `from`, or -1.
    int find_line(const std::vector<std::string> &lines, const std::string &text, std::size_t from = 0) {
        for (std::size_t i = from; i < lines.size(); ++i) {
            if (lines[i] == text) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int count_lines(const std::vector<std::string> &lines, const std::string &text) {
        int count = 0;
        for (const std::string &line : lines) {
            count += line == text ? 1 : 0;
        }
        return count;
    }

    std::string read_string(SturdyResult (*getter)(SturdyPlugin, char *, size_t, size_t *), SturdyPlugin plugin) {
        char buffer[128] = {};
        size_t length = 0;
        return getter(plugin, buffer, sizeof(buffer), &length) == STURDY_OK ? std::string{buffer} : std::string{"<error>"};
    }

    SturdyPluginLoadDesc desc_for(const char *path, uint32_t flags = 0, SturdyPlugin previous = {}) {
        SturdyPluginLoadDesc desc{};
        desc.struct_size = static_cast<uint32_t>(sizeof(SturdyPluginLoadDesc));
        desc.flags = flags;
        desc.path = path;
        desc.previous = previous;
        return desc;
    }

    struct Host final : SFT::Engine::GameLogic {
        SFT::Engine::GameLogicResult on_engine_initialized(SFT::Engine::Engine &) override { return {}; }
        std::optional<SFT::Engine::RenderFrameParameters> request_render_frame(SFT::Engine::Engine &, SFT::Core::RenderSurfaceHandle,
                                                                               const SFT::Core::FrameInput &) override {
            return std::nullopt;
        }
        void on_shutdown(SFT::Engine::Engine &) noexcept override {}
    };

} // namespace

/// Usage: PluginFfiTest [plugin-library [expected-language [all-callbacks]]]. With no arguments it tests the C fixture built with the engine; the
/// same checks run against any plugin that follows the fixture's logging protocol (see `fixtures/plugin_fixture.rs`).
int main(int argc, char **argv) {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    const char *const fixture = argc > 1 ? argv[1] : STURDY_PLUGIN_FIXTURE;
    const std::string expected_language = argc > 2 ? argv[2] : "c";
    // A plugin built with a safe layer installs every callback on every module; the hand-written fixtures leave some null.
    const bool all_callbacks = argc > 3 && std::string{argv[3]} == "all-callbacks";
    // One log per run so the runs may execute in parallel.
    g_log_path = std::filesystem::temp_directory_path() /
                 ("sturdy_plugin_ffi_test_" + expected_language + (all_callbacks ? "_safe" : "") + ".log");
    set_env("STURDY_PLUGIN_TEST_LOG", g_log_path.string().c_str());

    // ── Loading, describing, handing out ───────────────────────────────────────────────────────
    {
        clear_log();
        SturdyPlugin plugin{};
        SturdyPluginLoadDesc bad_size = desc_for(fixture);
        bad_size.struct_size = 1;
        check(sturdy_plugin_load(&bad_size, &plugin) == STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "a wrong desc size is rejected");
        const SturdyPluginLoadDesc empty = desc_for("");
        check(sturdy_plugin_load(&empty, &plugin) == STURDY_ERROR_INVALID_ARGUMENT, "an empty path is rejected");
        check(sturdy_plugin_load(nullptr, &plugin) == STURDY_ERROR_INVALID_ARGUMENT, "a null desc is rejected");
        const SturdyPluginLoadDesc missing = desc_for("/definitely/not/a/plugin.so");
        check(sturdy_plugin_load(&missing, &plugin) == STURDY_ERROR_NOT_AVAILABLE, "a missing library is reported as unavailable");
        check(sturdy_plugin_release(SturdyPlugin{12345}) != STURDY_OK, "releasing a handle that was never minted fails");

        const SturdyPluginLoadDesc desc = desc_for(fixture);
        check(sturdy_plugin_load(&desc, &plugin) == STURDY_OK, "the fixture plugin loads");
        check(read_string(sturdy_plugin_language, plugin) == expected_language, "its language was reported through set_info");
        check(read_string(sturdy_plugin_name, plugin) == "fixture", "...and its name");
        uint32_t count = 0;
        check(sturdy_plugin_module_count(plugin, &count) == STURDY_OK && count == 2, "it added two modules");
        char name[64] = {};
        size_t length = 0;
        check(sturdy_plugin_module_name(plugin, 1, name, sizeof(name), &length) == STURDY_OK && std::strcmp(name, "fixture.b") == 0,
              "module names read back in the order added");
        check(sturdy_plugin_module_name(plugin, 2, name, sizeof(name), &length) == STURDY_ERROR_OUT_OF_RANGE, "an index past the end is out of range");
        check(find_line(log_lines(), "unload state=set") < 0, "nothing was unloaded yet");

        SturdyGameModule modules[2];
        uint32_t taken = 0;
        check(sturdy_plugin_take_modules(plugin, modules, 1, &taken) == STURDY_ERROR_BUFFER_TOO_SMALL && taken == 2,
              "a too-small array reports the count and hands nothing out");
        check(sturdy_plugin_take_modules(plugin, modules, 2, &taken) == STURDY_OK && taken == 2, "the modules are handed out");
        check(sturdy_plugin_take_modules(plugin, modules, 2, &taken) == STURDY_ERROR_BUSY, "...once");
        check(std::strcmp(modules[0].name, "fixture.a") == 0 && modules[1].after_count == 1 && std::strcmp(modules[1].after[0], "fixture.a") == 0,
              "descriptors carry names and dependencies");
        if (!all_callbacks) {
            check(modules[0].logic.on_engine_initialized != nullptr && modules[1].logic.on_engine_initialized == nullptr,
                  "a callback the plugin did not supply stays null (not replaced by a trampoline)");
        }

        // Drive the callbacks the way the engine would.
        check(modules[0].logic.on_engine_initialized(SturdyEngine{0}, modules[0].logic.user_data) == STURDY_TRUE, "a module's initialize callback runs");
        modules[0].logic.on_shutdown(SturdyEngine{0}, modules[0].logic.user_data);

        // Releasing the handle while modules are out must not close the library.
        check(sturdy_plugin_release(plugin) == STURDY_OK, "the handle is released while modules are still out");
        check(sturdy_plugin_release(plugin) != STURDY_OK, "releasing twice is reported");
        check(find_line(log_lines(), "unload state=set") < 0, "the library stays open while a module is alive");
        modules[0].logic.destroy(modules[0].logic.user_data);
        check(find_line(log_lines(), "unload state=set") < 0, "...even after the first module is destroyed");
        modules[1].logic.destroy(modules[1].logic.user_data);
        const std::vector<std::string> lines = log_lines();
        const int destroy_b = find_line(lines, "destroy:fixture.b");
        const int unload = find_line(lines, "unload state=set");
        check(find_line(lines, "load path=" + std::string{fixture}) >= 0, "the entry point saw the library path");
        check(find_line(lines, "init:fixture.a") >= 0 && find_line(lines, "shutdown:fixture.a") >= 0, "callbacks reached the plugin");
        check(destroy_b >= 0 && unload > destroy_b, "UNLOAD ran once, after the last module was destroyed");
        check(count_lines(lines, "unload state=set") == 1, "...and exactly once");
    }

    // ── Releasing before the modules are handed out destroys them, then unloads ───────────────
    {
        clear_log();
        SturdyPlugin plugin{};
        const SturdyPluginLoadDesc desc = desc_for(fixture);
        check(sturdy_plugin_load(&desc, &plugin) == STURDY_OK, "load for the unused-plugin case");
        check(sturdy_plugin_release(plugin) == STURDY_OK, "release without ever starting it");
        const std::vector<std::string> lines = log_lines();
        check(find_line(lines, "destroy:fixture.a") >= 0 && find_line(lines, "destroy:fixture.b") >= 0, "modules never handed out are destroyed");
        check(find_line(lines, "unload state=set") > find_line(lines, "destroy:fixture.b"), "...before UNLOAD");
    }

    // ── A refused load destroys what it added and does not call UNLOAD ─────────────────────────
    {
        clear_log();
        set_env("STURDY_PLUGIN_TEST_REFUSE", "1");
        SturdyPlugin plugin{};
        const SturdyPluginLoadDesc desc = desc_for(fixture);
        check(sturdy_plugin_load(&desc, &plugin) == STURDY_ERROR_INITIALIZATION_FAILED, "an entry point that refuses fails the load");
        set_env("STURDY_PLUGIN_TEST_REFUSE", "0");
        const std::vector<std::string> lines = log_lines();
        check(find_line(lines, "destroy:fixture.a") >= 0 && find_line(lines, "destroy:fixture.b") >= 0, "modules added before the refusal are destroyed");
        check(find_line(lines, "unload state=set") < 0, "UNLOAD is not called for a refused load");
    }

    // ── Shadow copy ────────────────────────────────────────────────────────────────────────────
    {
        clear_log();
        SturdyPlugin plugin{};
        const SturdyPluginLoadDesc desc = desc_for(fixture, STURDY_PLUGIN_SHADOW_COPY);
        check(sturdy_plugin_load(&desc, &plugin) == STURDY_OK, "a shadow-copied plugin loads");
        std::string loaded_path;
        for (const std::string &line : log_lines()) {
            if (line.rfind("load path=", 0) == 0) {
                loaded_path = line.substr(10);
            }
        }
        check(!loaded_path.empty() && loaded_path != fixture && loaded_path.find("sturdy_plugin_") != std::string::npos,
              "the entry point saw the copy's path, not the original");
        check(std::filesystem::exists(loaded_path), "the copy exists while the plugin is loaded");
        check(sturdy_plugin_release(plugin) == STURDY_OK, "release the shadow-copied plugin");
        check(!std::filesystem::exists(loaded_path), "the copy is removed once the library is closed");
    }

    // ── In a running game, next to a C++ module; reload; state handoff ─────────────────────────
    {
        clear_log();
        SFT::Engine::Engine engine_object;
        const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
        const SturdyEngine engine{engine_handle.token()};
        uint32_t module_count = 0;

        {
            SFT::Engine::CompositeGameLogic composite;
            check(composite.add("host", std::make_unique<Host>()).has_value(), "add the C++ module");
            check(composite.on_engine_initialized(engine_object).has_value(), "the composite starts");

            SturdyPlugin first{};
            const SturdyPluginLoadDesc desc = desc_for(fixture);
            check(sturdy_plugin_load(&desc, &first) == STURDY_OK, "load generation 1");
            check(sturdy_plugin_request_load(engine, first) == STURDY_OK, "request its modules into the running game");
            check(sturdy_plugin_request_load(engine, first) == STURDY_ERROR_BUSY, "...but only once");
            engine_object.update(0.016);
            check(sturdy_module_count(engine, &module_count) == STURDY_OK && module_count == 3, "the plugin's two modules run beside the C++ one");
            check(count_lines(log_lines(), "init:fixture.a") == 1, "the plugin's module was initialized");

            // Generation 2 learns the first one's state, and replaces its modules in place.
            SturdyPlugin second{};
            const SturdyPluginLoadDesc reload_desc = desc_for(fixture, STURDY_PLUGIN_SHADOW_COPY, first);
            check(sturdy_plugin_load(&reload_desc, &second) == STURDY_OK, "load generation 2 over generation 1");
            check(find_line(log_lines(), "prior generation=1") >= 0, "generation 2 received generation 1's state");
            check(sturdy_plugin_request_reload(engine, first, second) == STURDY_OK, "request the reload");
            engine_object.update(0.016);
            std::vector<std::string> lines = log_lines();
            check(sturdy_module_count(engine, &module_count) == STURDY_OK && module_count == 3, "still three modules after the reload");
            check(count_lines(lines, "init:fixture.a") == 2 && count_lines(lines, "shutdown:fixture.a") == 1, "the old module shut down and the new one started");
            check(count_lines(lines, "destroy:fixture.a") == 1 && count_lines(lines, "destroy:fixture.b") == 1, "the old modules were destroyed once each");

            check(sturdy_plugin_release(first) == STURDY_OK, "release generation 1");
            lines = log_lines();
            check(count_lines(lines, "unload state=none") == 1, "generation 1 unloaded, with its state already handed on");
            check(count_lines(lines, "unload state=set") == 0, "...and generation 2 is still open");
            check(sturdy_plugin_release(second) == STURDY_OK, "release generation 2 (its modules are still running)");
            check(count_lines(log_lines(), "unload state=set") == 0, "generation 2 stays open while its modules run");

            composite.on_shutdown(engine_object);
        } // the composite is destroyed here, destroying the modules and so closing generation 2

        const std::vector<std::string> lines = log_lines();
        check(count_lines(lines, "destroy:fixture.a") == 2 && count_lines(lines, "destroy:fixture.b") == 2, "every module was destroyed exactly once");
        check(count_lines(lines, "unload state=set") == 1 && find_line(lines, "unload generation=2") >= 0, "generation 2 unloaded last, holding generation 2's state");
    }

    SFT::Async::Scheduler::shutdown();
    clear_log();
    if (failures != 0) {
        (void)std::fprintf(stderr, "PluginFfiTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
