#include <Engine/ForeignEcs.hpp>

#include <cstring>
#include <format>

#include <Engine/EcsReflection.hpp>

namespace SFT::Engine {

    std::optional<std::string> validate_foreign_component(std::string_view name, u32 size, u32 align,
                                                          const Ecs::ComponentSchema *schema) {
        if (name.empty()) {
            return std::string{"component name must not be empty"};
        }
        // Zero-sized components would need archetype columns with no storage, a shape the rest of the ECS does not currently
        // produce. Refusing is better than being the first to create it.
        if (size == 0) {
            return std::string{"component size must be nonzero; tag components are not supported yet"};
        }
        if (align == 0 || (align & (align - 1)) != 0 || align > 64) {
            return std::string{"component alignment must be a power of two no greater than 64"};
        }
        if (schema != nullptr && !schema->fields.empty()) {
            if (const auto reason = Ecs::validate_component_fields(size, schema->fields)) {
                return std::format("invalid field layout for component '{}': {}", name, *reason);
            }
        }
        return std::nullopt;
    }

    std::expected<Ecs::ComponentId, std::string> register_foreign_component(Ecs::ComponentRegistry &registry,
                                                                            std::string_view name,
                                                                            u32 size,
                                                                            u32 align,
                                                                            const Ecs::ComponentSchema *schema) {
        if (auto invalid = validate_foreign_component(name, size, align, schema)) {
            return std::unexpected(std::move(*invalid));
        }
        const bool has_fields = schema != nullptr && !schema->fields.empty();

        // Registering the same component twice is expected: a binding calls this on every run, and possibly from several places.
        // Returning the existing id keeps that idempotent, but only when the layout matches.
        if (const auto existing = registry.find(ustr{name})) {
            const Ecs::ComponentInfo *descriptor = registry.info(*existing);
            if (descriptor != nullptr && (descriptor->size != size || descriptor->align != align)) {
                return std::unexpected(std::string{"a component with this name is already registered with a different layout"});
            }
            // Fallback for a C++ component whose shape was never attached (`ensure_reflected` normally does it): if a reflection
            // type of the same name is registered by now, derive the shape from it. Reflection registers a type on first use,
            // so a C++ module should call `ensure_reflected<T>` to make its component's shape known.
            if (has_fields && descriptor != nullptr && !descriptor->layout) {
                const UString type_name{name};
                if (const Reflection::TypeInfo *type = Reflection::TypeRegistry::instance().find(type_name.as_ustr());
                    type != nullptr && type->size == descriptor->size && type->align == descriptor->align) {
                    (void)attach_reflected_layout(registry, *existing, *type);
                }
            }
            // Same size and alignment is not the same component: a Rust {f32, f32} and a C# {u32, u32} are both 8 bytes.
            if (has_fields && descriptor != nullptr && descriptor->layout &&
                Ecs::fingerprint_component_layout(size, align, schema->fields) != descriptor->layout) {
                return std::unexpected(std::format(
                    "component '{}' is already registered with a different field layout: {}", name,
                    Ecs::describe_layout_difference(descriptor->size, descriptor->align, descriptor->schema.fields, size, align,
                                                    schema->fields)));
            }
            return *existing;
        }

        Ecs::ComponentInfo info{};
        info.key = Ecs::ComponentKey::from_name(name);
        info.canonical_name = UString{std::string{name}};
        info.schema_version = 1;
        info.size = size;
        info.align = align;
        // FfiBlittable records where this component came from; the trivially-* flags are what the erased World API actually
        // checks before copying bytes.
        info.flags = Ecs::ComponentFlags::TriviallyCopyable | Ecs::ComponentFlags::TriviallyDestructible |
                     Ecs::ComponentFlags::FfiBlittable;
        info.default_construct = [](void *destination, void *user_data) noexcept {
            // Size travels through user_data because these are plain function pointers with no capture; zeroing gives a
            // foreign component a defined initial value.
            std::memset(destination, 0, reinterpret_cast<usize>(user_data));
        };
        info.copy_construct = [](void *destination, const void *source, void *user_data) noexcept {
            std::memcpy(destination, source, reinterpret_cast<usize>(user_data));
        };
        info.move_construct = [](void *destination, void *source, void *user_data) noexcept {
            std::memcpy(destination, source, reinterpret_cast<usize>(user_data));
        };
        info.destroy = [](void *, void *) noexcept {};
        info.user_data = reinterpret_cast<void *>(static_cast<usize>(size));
        if (has_fields) {
            info.schema = *schema;
        }

        auto registered = registry.register_component(std::move(info));
        if (!registered) {
            return std::unexpected(registered.error().message.cpp_string());
        }
        return *registered;
    }

} // namespace SFT::Engine
