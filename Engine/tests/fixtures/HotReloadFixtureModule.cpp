/// A real hot-reloadable module, compiled twice (`FIXTURE_VERSION=1` and `=2`, see
/// `Engine/CMakeLists.txt`) into two distinct shared libraries that `HotReloadTest.cpp` loads,
/// unloads, and reloads for real — the actual DLL load/reload mechanics, not a simulation of
/// them. V2 adds a field to the same reflected type (`level`), standing in for "the game logic
/// got rebuilt with a real code change" across a reload.

#include <Engine/HotReloadableModule.hpp>
#include <Reflection/Reflection.hpp>

namespace {

    struct FixtureItem {
        int power = 0;
#if FIXTURE_VERSION >= 2
        int level = 0;
#endif
    };

} // namespace

SFT_REFLECT_TYPE(FixtureItem, "hotreload.fixture_item");
SFT_REFLECT_FIELD(power);
#if FIXTURE_VERSION >= 2
SFT_REFLECT_FIELD(level);
#endif
SFT_REFLECT_END();

namespace {

    void register_fixture_types(SFT::Reflection::TypeRegistry &registry) {
        (void)registry.try_register<FixtureItem>();
    }

    void unregister_fixture_types(SFT::Reflection::TypeRegistry &registry) {
        (void)registry.unregister_type(SFT::Reflection::type_id_for<FixtureItem>());
    }

} // namespace

SFT_HOT_RELOAD_MODULE_EXPORTS(register_fixture_types, unregister_fixture_types, FIXTURE_VERSION)
