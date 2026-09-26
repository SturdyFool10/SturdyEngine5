#include <Engine/HotReloadableModule.hpp>

#include <memory>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] HotReloadError hot_reload_error(HotReloadErrorCode code, UString message) {
            return HotReloadError{.code = code, .message = std::move(message)};
        }

        /// What the C facade's opaque builder pointer actually is.
        struct CBuilder {
            CBuilder(Reflection::TypeRegistry *target, std::string_view name, u32 size, u32 align)
                : registry(target), builder(name, size, align) {}
            Reflection::TypeRegistry *registry;
            Reflection::TypeInfoBuilder builder;
        };

        [[nodiscard]] Reflection::PrimitiveKind primitive_kind_from_c(u32 value) noexcept {
            switch (value) {
                case 1: return Reflection::PrimitiveKind::Bool;
                case 2: return Reflection::PrimitiveKind::SignedInt;
                case 3: return Reflection::PrimitiveKind::UnsignedInt;
                case 4: return Reflection::PrimitiveKind::Float;
                default: return Reflection::PrimitiveKind::None;
            }
        }

        void *c_builder_create(void *registry, const char *name, u32 size, u32 align) {
            if (registry == nullptr || name == nullptr) {
                return nullptr;
            }
            return new CBuilder(static_cast<Reflection::TypeRegistry *>(registry), name, size, align);
        }

        void c_builder_set_constructors(void *builder,
                                        void (*move_construct)(void *, void *, void *),
                                        void (*destroy)(void *, void *),
                                        void (*default_construct)(void *, void *),
                                        void *user_data) {
            if (builder == nullptr) {
                return;
            }
            static_cast<CBuilder *>(builder)->builder.constructors(
                reinterpret_cast<Reflection::TypeMoveConstructFn>(move_construct),
                reinterpret_cast<Reflection::TypeDestroyFn>(destroy),
                reinterpret_cast<Reflection::TypeDefaultConstructFn>(default_construct), nullptr, user_data);
        }

        void c_builder_add_field(void *builder, const char *name, u32 offset, u32 size, u32 align, u32 primitive_kind) {
            if (builder == nullptr || name == nullptr) {
                return;
            }
            static_cast<CBuilder *>(builder)->builder.field(name, offset, size, align, Reflection::TypeId{}, Reflection::FieldFlags::Trivial, nullptr,
                                                             nullptr, {}, primitive_kind_from_c(primitive_kind));
        }

        int c_builder_finish(void *builder) {
            if (builder == nullptr) {
                return 0;
            }
            const std::unique_ptr<CBuilder> owned{static_cast<CBuilder *>(builder)};
            return owned->registry->register_type(owned->builder.build()).has_value() ? 1 : 0;
        }

        void c_builder_discard(void *builder) { delete static_cast<CBuilder *>(builder); }

        int c_unregister_type(void *registry, const char *name) {
            if (registry == nullptr || name == nullptr) {
                return 0;
            }
            return static_cast<Reflection::TypeRegistry *>(registry)->unregister_type(Reflection::TypeId::from_name(name)) ? 1 : 0;
        }

        [[nodiscard]] SturdyModuleReflectionApi make_c_api(Reflection::TypeRegistry &registry) {
            return SturdyModuleReflectionApi{
                .struct_size = sizeof(SturdyModuleReflectionApi),
                .abi_version = 1,
                .registry = &registry,
                .type_builder_create = &c_builder_create,
                .type_builder_set_constructors = &c_builder_set_constructors,
                .type_builder_add_field = &c_builder_add_field,
                .type_builder_finish = &c_builder_finish,
                .type_builder_discard = &c_builder_discard,
                .unregister_type = &c_unregister_type,
            };
        }

    } // namespace

    HotReloadExpected<void> HotReloadableModule::load(std::string_view path, Reflection::TypeRegistry &registry) {
        Foundation::DynamicLibrary library;
        if (!library.load(path)) {
            return std::unexpected(hot_reload_error(HotReloadErrorCode::LibraryLoadFailed, library.last_error()));
        }

        auto *register_types_fn = library.symbol_as<ModuleRegisterTypesFn>("sturdy_module_register_types");
        auto *unregister_types_fn = library.symbol_as<ModuleUnregisterTypesFn>("sturdy_module_unregister_types");
        auto *register_types_c_fn = library.symbol_as<ModuleRegisterTypesCFn>("sturdy_module_register_types_c");
        auto *unregister_types_c_fn = library.symbol_as<ModuleUnregisterTypesCFn>("sturdy_module_unregister_types_c");
        auto *version_fn = library.symbol_as<ModuleVersionFn>("sturdy_module_version");
        const bool has_cpp_entry_points = register_types_fn != nullptr && unregister_types_fn != nullptr;
        const bool has_c_entry_points = register_types_c_fn != nullptr && unregister_types_c_fn != nullptr;
        if ((!has_cpp_entry_points && !has_c_entry_points) || version_fn == nullptr) {
            return std::unexpected(hot_reload_error(
                HotReloadErrorCode::MissingEntryPoint,
                UString{"module is missing sturdy_module_version or a register/unregister pair "
                        "(sturdy_module_register_types/unregister_types, or the _c variants)"}));
        }

        if (has_cpp_entry_points) {
            register_types_fn(&registry);
            register_types_c_fn = nullptr;
            unregister_types_c_fn = nullptr;
        } else {
            const SturdyModuleReflectionApi api = make_c_api(registry);
            register_types_c_fn(&api);
            register_types_fn = nullptr;
            unregister_types_fn = nullptr;
        }

        // Only commit to member state once everything above succeeded — a failed load() must
        // leave this object exactly as it was (still unloaded, or still holding the previous
        // module), never partially updated.
        library_ = std::move(library);
        register_types_fn_ = register_types_fn;
        unregister_types_fn_ = unregister_types_fn;
        register_types_c_fn_ = register_types_c_fn;
        unregister_types_c_fn_ = unregister_types_c_fn;
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
        } else if (unregister_types_c_fn_ != nullptr && registered_against_ != nullptr) {
            const SturdyModuleReflectionApi api = make_c_api(*registered_against_);
            unregister_types_c_fn_(&api);
        }
        library_.unload();
        register_types_fn_ = nullptr;
        unregister_types_fn_ = nullptr;
        register_types_c_fn_ = nullptr;
        unregister_types_c_fn_ = nullptr;
        version_fn_ = nullptr;
        registered_against_ = nullptr;
    }

    HotReloadExpected<void> HotReloadableModule::reload(std::string_view new_path, Reflection::TypeRegistry &registry) {
        unload();
        return load(new_path, registry);
    }

} // namespace SFT::Engine
