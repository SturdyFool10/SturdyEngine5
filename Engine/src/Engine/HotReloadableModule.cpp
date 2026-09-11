#include <Engine/HotReloadableModule.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] HotReloadError hot_reload_error(HotReloadErrorCode code, UString message) {
            return HotReloadError{.code = code, .message = std::move(message)};
        }

    } // namespace

    HotReloadExpected<void> HotReloadableModule::load(std::string_view path, Reflection::TypeRegistry &registry) {
        Foundation::DynamicLibrary library;
        if (!library.load(path)) {
            return std::unexpected(hot_reload_error(HotReloadErrorCode::LibraryLoadFailed, library.last_error()));
        }

        auto *register_types_fn = library.symbol_as<ModuleRegisterTypesFn>("sturdy_module_register_types");
        auto *unregister_types_fn = library.symbol_as<ModuleUnregisterTypesFn>("sturdy_module_unregister_types");
        auto *version_fn = library.symbol_as<ModuleVersionFn>("sturdy_module_version");
        if (register_types_fn == nullptr || unregister_types_fn == nullptr || version_fn == nullptr) {
            return std::unexpected(hot_reload_error(
                HotReloadErrorCode::MissingEntryPoint,
                UString{"module is missing one of sturdy_module_register_types/unregister_types/version"}));
        }

        register_types_fn(&registry);

        // Only commit to member state once everything above succeeded — a failed load() must
        // leave this object exactly as it was (still unloaded, or still holding the previous
        // module), never partially updated.
        library_ = std::move(library);
        register_types_fn_ = register_types_fn;
        unregister_types_fn_ = unregister_types_fn;
        version_fn_ = version_fn;
        registered_against_ = &registry;
        return {};
    }

    void HotReloadableModule::unload() {
        if (!library_.is_loaded()) {
            return;
        }
        if (unregister_types_fn_ != nullptr && registered_against_ != nullptr) {
            unregister_types_fn_(registered_against_);
        }
        library_.unload();
        register_types_fn_ = nullptr;
        unregister_types_fn_ = nullptr;
        version_fn_ = nullptr;
        registered_against_ = nullptr;
    }

    HotReloadExpected<void> HotReloadableModule::reload(std::string_view new_path, Reflection::TypeRegistry &registry) {
        unload();
        return load(new_path, registry);
    }

} // namespace SFT::Engine
