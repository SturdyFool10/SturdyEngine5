/// End-to-end coverage of the hot-reload primitives against two *really compiled, separately
/// built* shared libraries (see `Engine/tests/fixtures/HotReloadFixtureModule.cpp` and the
/// `STURDY_HOT_RELOAD_FIXTURE_V1`/`_V2` paths `Engine/CMakeLists.txt` bakes in) — real
/// `dlopen`/`dlsym`/`dlclose` (or the Windows equivalents) through `Foundation::DynamicLibrary`,
/// real reflection registration/unregistration crossing a real shared-library boundary through
/// `Engine::HotReloadableModule`, and `Engine::HotReloadWatcher`'s mtime-based change detection.

#include <Engine/HotReloadWatcher.hpp>
#include <Engine/HotReloadableModule.hpp>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "HotReloadTest: %s\n", description);
            ++failures;
        }
    }

} // namespace

int main() {
    using namespace SFT;
    using namespace SFT::Engine;

    Reflection::TypeRegistry &registry = Reflection::TypeRegistry::instance();
    const UString fixture_type_name{"hotreload.fixture_item"};

    // ── Loading a module that does not exist: clean error, no partial state ──────────────────────
    {
        HotReloadableModule module;
        const auto failed_load = module.load("/definitely/not/a/real/path.so", registry);
        check(!failed_load.has_value(), "loading a nonexistent module path must fail");
        check(!failed_load.has_value() || failed_load.error().code == HotReloadErrorCode::LibraryLoadFailed,
              "a nonexistent path must be reported as LibraryLoadFailed");
        check(!module.is_loaded(), "a failed load must leave the module not-loaded");
    }

    // ── Loading a real module (V1), and that it really registered into TypeRegistry ─────────────
    HotReloadableModule module;
    const auto loaded_v1 = module.load(STURDY_HOT_RELOAD_FIXTURE_V1, registry);
    check(loaded_v1.has_value(), "loading the real V1 fixture module must succeed");
    check(module.is_loaded(), "the module must report itself loaded after a successful load");
    check(module.version() == 1, "the loaded module's self-reported version must be 1");

    const Reflection::TypeInfo *v1_type = registry.find(fixture_type_name);
    check(v1_type != nullptr, "V1's register_types must have registered hotreload.fixture_item");
    check(v1_type != nullptr && v1_type->fields.size() == 1, "V1's FixtureItem must have exactly one field (power)");
    check(v1_type != nullptr && v1_type->find_field("power") != nullptr, "V1's FixtureItem must declare 'power'");

    // ── Reloading to V2: old unregisters, new registers, and the type's real shape changed ───────
    const auto reloaded = module.reload(STURDY_HOT_RELOAD_FIXTURE_V2, registry);
    check(reloaded.has_value(), "reloading to the real V2 fixture module must succeed");
    check(module.version() == 2, "after reload, the module's self-reported version must be 2");

    const Reflection::TypeInfo *v2_type = registry.find(fixture_type_name);
    check(v2_type != nullptr, "V2's register_types must have re-registered hotreload.fixture_item");
    check(v2_type != nullptr && v2_type->fields.size() == 2,
          "V2's FixtureItem must have two fields (power, level) -- proof the reload actually "
          "replaced the old registration with the rebuilt module's real shape, not a stale copy");
    check(v2_type != nullptr && v2_type->find_field("level") != nullptr,
          "V2's FixtureItem must declare 'level', which V1 never had");

    // ── Unloading: the type must become unreachable again ────────────────────────────────────────
    module.unload();
    check(!module.is_loaded(), "the module must report itself not-loaded after unload");
    check(registry.find(fixture_type_name) == nullptr,
          "after unload, hotreload.fixture_item must no longer be findable -- its unregister_types "
          "ran, and its function pointers (which pointed into the now-unloaded .so) are gone with it");

    // ── HotReloadWatcher: mtime-based change detection on a real file ────────────────────────────
    const std::filesystem::path watch_path = std::filesystem::temp_directory_path() / "sturdy_hot_reload_watch_test.txt";
    {
        std::ofstream initial(watch_path);
        initial << "v1";
    }

    HotReloadWatcher watcher(watch_path);
    check(!watcher.poll(), "the first poll after construction must not report a change (baseline was primed)");

    // Ensure the new mtime differs even on filesystems with coarse (e.g. 1s) mtime resolution.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    {
        std::ofstream rewritten(watch_path);
        rewritten << "v2, now longer";
    }
    check(watcher.poll(), "a poll after the watched file's mtime changed must report a change");
    check(!watcher.poll(), "a poll immediately after a reported change (no further write) must not report one again");

    std::filesystem::remove(watch_path);

    if (failures != 0) {
        (void)std::fprintf(stderr, "HotReloadTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
