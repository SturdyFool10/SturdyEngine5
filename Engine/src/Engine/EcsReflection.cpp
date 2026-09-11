#include <Engine/EcsReflection.hpp>

#include <vector>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] EcsReflectionBridgeExpected<std::vector<std::byte>> read_whole_component(Ecs::World &world,
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

    EcsReflectionBridgeExpected<void> read_component_field(Ecs::World &world,
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
