#include <Engine/EcsReflection.hpp>

#include <algorithm>
#include <vector>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] EcsReflectionBridgeExpected<std::vector<std::byte>> read_whole_component(const Ecs::World &world,
                                                                                                Ecs::Entity entity,
                                                                                                Ecs::ComponentId component,
                                                                                                const Reflection::TypeInfo &type) {
            std::vector<std::byte> scratch(type.size);
            auto read = world.read_component_erased(entity, component, scratch.data(), scratch.size());
            if (!read) {
                return std::unexpected(EcsReflectionBridgeError{
                    .code = EcsReflectionBridgeErrorCode::WorldAccessFailed,
                    .message = read.error().message,
                    .world_error = read.error().code,
                });
            }
            return scratch;
        }

    } // namespace

    EcsReflectionBridgeExpected<std::pair<Ecs::ComponentId, Reflection::TypeId>> ensure_reflected(
        Ecs::ComponentRegistry &components, std::string_view component_name, std::string_view type_name) {
        const UString component_ustring{component_name};
        const auto component_id = components.find(component_ustring.as_ustr());
        if (!component_id) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::ComponentNotRegistered,
                .message = UString{"no ECS component is registered under that name"},
                .world_error = std::nullopt,
            });
        }
        const UString type_ustring{type_name};
        const Reflection::TypeInfo *type = Reflection::TypeRegistry::instance().find(type_ustring.as_ustr());
        if (type == nullptr) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::TypeNotRegistered,
                .message = UString{"no reflection type is registered under that name"},
                .world_error = std::nullopt,
            });
        }
        const Ecs::ComponentInfo *component_info = components.info(*component_id);
        if (component_info == nullptr || component_info->size != type->size || component_info->align != type->align) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::DescriptorMismatch,
                .message = UString{"the ECS component and the reflection type disagree on size/align"},
                .world_error = std::nullopt,
            });
        }
        if (auto attached = attach_reflected_layout(components, *component_id, *type); !attached) {
            return std::unexpected(attached.error());
        }
        return std::pair{*component_id, type->key};
    }

    std::optional<Ecs::ComponentSchema> derive_component_schema(const Reflection::TypeInfo &type) {
        if (static_cast<bool>(type.base_type) || !type.secondary_bases.empty() || type.fields.empty() || type.size == 0 || type.align == 0) {
            return std::nullopt;
        }
        std::vector<const Reflection::FieldInfo *> ordered;
        ordered.reserve(type.fields.size());
        for (const Reflection::FieldInfo &field : type.fields) {
            ordered.push_back(&field);
        }
        std::ranges::sort(ordered, [](const Reflection::FieldInfo *a, const Reflection::FieldInfo *b) { return a->offset < b->offset; });

        const auto kind_of = [](const Reflection::FieldInfo &field) -> std::optional<Ecs::FieldKind> {
            if (!has_flag(field.flags, Reflection::FieldFlags::Trivial) || has_flag(field.flags, Reflection::FieldFlags::Static)) {
                return std::nullopt;
            }
            switch (field.primitive_kind) {
                case Reflection::PrimitiveKind::Bool:
                    return field.size == 1 ? std::optional{Ecs::FieldKind::Bool} : std::nullopt;
                case Reflection::PrimitiveKind::SignedInt:
                    switch (field.size) {
                        case 1: return Ecs::FieldKind::I8;
                        case 2: return Ecs::FieldKind::I16;
                        case 4: return Ecs::FieldKind::I32;
                        case 8: return Ecs::FieldKind::I64;
                        default: return std::nullopt;
                    }
                case Reflection::PrimitiveKind::UnsignedInt:
                    switch (field.size) {
                        case 1: return Ecs::FieldKind::U8;
                        case 2: return Ecs::FieldKind::U16;
                        case 4: return Ecs::FieldKind::U32;
                        case 8: return Ecs::FieldKind::U64;
                        default: return std::nullopt;
                    }
                case Reflection::PrimitiveKind::Float:
                    switch (field.size) {
                        case 4: return Ecs::FieldKind::F32;
                        case 8: return Ecs::FieldKind::F64;
                        default: return std::nullopt;
                    }
                case Reflection::PrimitiveKind::None: return std::nullopt;
            }
            return std::nullopt;
        };

        Ecs::ComponentSchema schema;
        usize cursor = 0;
        for (const Reflection::FieldInfo *field : ordered) {
            const auto kind = kind_of(*field);
            if (!kind) {
                return std::nullopt;
            }
            const usize kind_size = Ecs::field_kind_size(*kind);
            // Natural alignment only: anything else (packing, an unreflected member in between) is not a layout we can vouch for.
            const usize expected_offset = (cursor + kind_size - 1) / kind_size * kind_size;
            if (field->offset != expected_offset || field->align != kind_size) {
                return std::nullopt;
            }
            schema.fields.push_back(Ecs::ComponentField{.name = field->name.cpp_string(),
                                                        .kind = *kind,
                                                        .offset = static_cast<u32>(field->offset),
                                                        .count = 1});
            cursor = field->offset + kind_size;
        }
        // The struct's own alignment pads its tail; nothing else may be left over.
        const usize end = (cursor + type.align - 1) / type.align * type.align;
        if (end != type.size) {
            return std::nullopt;
        }
        return schema;
    }

    EcsReflectionBridgeExpected<void> attach_reflected_layout(Ecs::ComponentRegistry &components, Ecs::ComponentId id,
                                                              const Reflection::TypeInfo &type) {
        std::optional<Ecs::ComponentSchema> schema = derive_component_schema(type);
        if (!schema) {
            return {};
        }
        if (auto described = components.describe_layout(id, std::move(*schema)); !described) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::DescriptorMismatch,
                .message = described.error().message,
                .world_error = std::nullopt,
            });
        }
        return {};
    }

    EcsReflectionBridgeExpected<void> read_component_field(const Ecs::World &world,
                                                            Ecs::Entity entity,
                                                            Ecs::ComponentId component,
                                                            const Reflection::TypeInfo &type,
                                                            std::string_view field_name,
                                                            void *out_data,
                                                            usize size) {
        const Reflection::FieldInfo *field = Reflection::TypeRegistry::instance().find_field(type, field_name);
        if (field == nullptr) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::FieldNotFound,
                .message = UString{"no such field on this type or its reflected base chain"},
                .world_error = std::nullopt,
            });
        }

        auto scratch = read_whole_component(world, entity, component, type);
        if (!scratch) {
            return std::unexpected(scratch.error());
        }

        if (!Reflection::copy_field_out(*field, scratch->data(), out_data, size)) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::FieldAccessFailed,
                .message = UString{"field size mismatch or field has no readable representation"},
                .world_error = std::nullopt,
            });
        }
        return {};
    }

    EcsReflectionBridgeExpected<void> write_component_field(Ecs::World &world,
                                                             Ecs::Entity entity,
                                                             Ecs::ComponentId component,
                                                             const Reflection::TypeInfo &type,
                                                             std::string_view field_name,
                                                             const void *in_data,
                                                             usize size) {
        const Reflection::FieldInfo *field = Reflection::TypeRegistry::instance().find_field(type, field_name);
        if (field == nullptr) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::FieldNotFound,
                .message = UString{"no such field on this type or its reflected base chain"},
                .world_error = std::nullopt,
            });
        }

        auto scratch = read_whole_component(world, entity, component, type);
        if (!scratch) {
            return std::unexpected(scratch.error());
        }

        if (!Reflection::copy_field_in(*field, scratch->data(), in_data, size)) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::FieldAccessFailed,
                .message = UString{"field size mismatch, field is ReadOnly, or field has no writable representation"},
                .world_error = std::nullopt,
            });
        }

        auto written = world.write_component_erased(entity, component, scratch->data(), scratch->size());
        if (!written) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::WorldAccessFailed,
                .message = written.error().message,
                .world_error = written.error().code,
            });
        }
        return {};
    }

} // namespace SFT::Engine
