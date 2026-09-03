/// C ABI implementation of the rendering surface: asset creation and scene composition.
///
/// Rendering in this engine is driven from the ECS. An entity carrying a `WorldTransform` and a
/// `ModelRenderer` is drawn; one carrying a transform and a light component lights the scene. All
/// of those components are trivially copyable, so a foreign caller could in principle write them
/// through `sturdy_ecs_add_component` — but only by hard-coding their memory layout, which would
/// break silently the first time a field moved. The `sturdy_render_set_*` family exists so the
/// layout stays the engine's business: the caller passes semantic values and this layer builds the
/// component.
///
/// Assets travel as opaque bytes rather than through a handle registry. `Engine::Asset` is trivially
/// copyable and standard layout — the engine static_asserts both — so copying it into a fixed-size
/// opaque struct avoids inventing a second lifetime to manage on top of the one the asset manager
/// already has.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/Engine.hpp>
#include <Renderer/Mesh.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    using SFT::f32;
    using SFT::u32;

    // If `Engine::Asset` ever outgrows the opaque blob, this fails at compile time rather than
    // silently truncating an asset handle into something that no longer identifies anything.
    static_assert(sizeof(SFT::Engine::Asset) <= sizeof(SturdyAsset),
                  "SturdyAsset is too small to carry an Engine::Asset");
    static_assert(std::is_trivially_copyable_v<SFT::Engine::Asset>,
                  "Engine::Asset must stay trivially copyable to travel as opaque bytes");

    /// Copies an engine asset into its opaque ABI representation.
    ///
    /// @param asset Engine-side asset.
    ///
    /// @return The opaque value, zero-padded beyond the asset's own bytes.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyAsset to_abi_asset(const SFT::Engine::Asset &asset) noexcept {
        SturdyAsset result{};
        std::memcpy(&result, &asset, sizeof(asset));
        return result;
    }

    /// Copies an opaque ABI asset back into its engine representation.
    ///
    /// @param asset Value received from the caller.
    ///
    /// @return The engine-side asset. A zeroed input yields an invalid asset, which every asset
    ///         manager entry point rejects on its own.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Engine::Asset to_engine_asset(const SturdyAsset &asset) noexcept {
        SFT::Engine::Asset result{};
        std::memcpy(&result, &asset, sizeof(result));
        return result;
    }

    /// Resolves an engine handle to its asset manager.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_engine Receives the borrowed engine on success.
    ///
    /// @return `STURDY_OK`, or the handle failure encountered.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve(SturdyEngine engine, SFT::Engine::Engine **out_engine) noexcept {
        return resolve_engine(engine, out_engine);
    }

    /// Reports whether every supplied value is finite.
    ///
    /// @param values Values to inspect.
    ///
    /// @return Returns `true` when every value is finite; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool all_finite(std::initializer_list<f32> values) noexcept {
        for (const f32 value : values) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    /// Builds the mesh for a built-in shape.
    ///
    /// @param shape Shape to generate.
    /// @param params Dimensions.
    /// @param label Debug label, or null.
    /// @param out_mesh Receives the mesh.
    ///
    /// @return `STURDY_OK`, or why the shape could not be generated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SturdyResult build_shape(SturdyShape shape,
                                           const SturdyShapeParams &params,
                                           const char *label,
                                           SFT::Renderer::Mesh *out_mesh) {
        namespace R = SFT::Renderer;
        switch (shape) {
        case STURDY_SHAPE_CUBE:
            *out_mesh = R::Mesh::cube(R::CubeParams{.size = params.size}, label);
            return STURDY_OK;
        case STURDY_SHAPE_BOX:
            *out_mesh = R::Mesh::rectangular_prism(
                R::RectangularPrismParams{
                    .extents = glm::vec3{params.extents_x, params.extents_y, params.extents_z}},
                label);
            return STURDY_OK;
        case STURDY_SHAPE_UV_SPHERE:
            *out_mesh = R::Mesh::uv_sphere(
                R::UvSphereParams{.radius = params.radius, .rings = params.rings, .segments = params.segments},
                label);
            return STURDY_OK;
        case STURDY_SHAPE_ICO_SPHERE:
            *out_mesh = R::Mesh::ico_sphere(
                R::IcoSphereParams{.radius = params.radius, .subdivisions = params.subdivisions}, label);
            return STURDY_OK;
        case STURDY_SHAPE_PLANE:
            *out_mesh = R::Mesh::plane(R::PlaneParams{.width = params.width,
                                                      .depth = params.depth,
                                                      .width_segments = params.width_segments,
                                                      .depth_segments = params.depth_segments},
                                       label);
            return STURDY_OK;
        case STURDY_SHAPE_CYLINDER:
            *out_mesh = R::Mesh::cylinder(R::CylinderParams{.radius = params.radius,
                                                            .height = params.height,
                                                            .radial_segments = params.radial_segments,
                                                            .capped = params.capped != STURDY_FALSE},
                                          label);
            return STURDY_OK;
        case STURDY_SHAPE_CONE:
            *out_mesh = R::Mesh::cone(R::ConeParams{.radius = params.radius,
                                                    .height = params.height,
                                                    .radial_segments = params.radial_segments,
                                                    .capped = params.capped != STURDY_FALSE},
                                      label);
            return STURDY_OK;
        case STURDY_SHAPE_TORUS:
            *out_mesh = R::Mesh::torus(R::TorusParams{.major_radius = params.major_radius,
                                                      .minor_radius = params.minor_radius,
                                                      .major_segments = params.major_segments,
                                                      .minor_segments = params.minor_segments},
                                       label);
            return STURDY_OK;
        case STURDY_SHAPE_TETRAHEDRON:
            *out_mesh = R::Mesh::tetrahedron(R::TetrahedronParams{.size = params.size}, label);
            return STURDY_OK;
        case STURDY_SHAPE_FORCE_U32:
        default:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized shape");
        }
    }

    /// Attaches or replaces a trivially-copyable engine component on an entity.
    ///
    /// Routed through the erased world API so a dead entity is reported rather than terminating the
    /// process, exactly as the rest of the ECS surface does.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param entity Entity to modify.
    /// @param value Component value to store.
    ///
    /// @return `STURDY_OK`, or the failure encountered.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <class Component>
    [[nodiscard]] SturdyResult set_component(SturdyEngine engine, SturdyEntity entity, const Component &value) {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::World &world = resolved_engine->ecs_world();
        const SFT::Ecs::ComponentId id = world.registry().component<Component>();
        const SFT::Ecs::Entity target{.index = entity.index, .generation = entity.generation};

        // Replace rather than fail when the component is already there: these are setters, and a
        // caller repositioning an entity every frame should not have to track whether it is the
        // first time.
        if (world.has_component_erased(target, id)) {
            auto written = world.write_component_erased(target, id, &value, sizeof(Component));
            if (!written) {
                return set_error(written.error().code == SFT::Ecs::WorldErasedErrorCode::DeadEntity
                                     ? STURDY_ERROR_ENTITY_NOT_ALIVE
                                     : STURDY_ERROR_INVALID_ARGUMENT,
                                 written.error().message.cpp_string_view());
            }
            return STURDY_OK;
        }

        auto added = world.add_component_erased(target, id, &value);
        if (!added) {
            return set_error(added.error().code == SFT::Ecs::WorldErasedErrorCode::DeadEntity
                                 ? STURDY_ERROR_ENTITY_NOT_ALIVE
                                 : STURDY_ERROR_INVALID_ARGUMENT,
                             added.error().message.cpp_string_view());
        }
        return STURDY_OK;
    }

} // namespace

namespace SFT::Ffi {

    /// Registers the render-extraction systems a rendered frame depends on.
    ///
    /// @param engine Engine to configure.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void install_render_extraction(SFT::Engine::Engine &engine) {
        // Mirrors what a C++ product writes for itself. These have to be typed systems: they take
        // component references and a WriteResource, which is exactly the shape the erased system API
        // cannot express, so a foreign caller could not register them even in principle.
        SFT::Ecs::World &world = engine.ecs_world();

        world.bind_resource(engine.render_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](SFT::Ecs::Entity entity,
               const SFT::Engine::WorldTransform &transform,
               const SFT::Engine::ModelRenderer &model_renderer,
               SFT::Ecs::WriteResource<SFT::Engine::RenderFrameRequests> render) noexcept {
                render->submit(entity, transform, model_renderer);
            });

        world.bind_resource(engine.light_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](SFT::Ecs::Entity entity,
               const SFT::Engine::WorldTransform &transform,
               const SFT::Engine::DirectionalLightRenderer &light,
               SFT::Ecs::WriteResource<SFT::Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
        engine.render_extraction_schedule().add_system(
            [](SFT::Ecs::Entity entity,
               const SFT::Engine::WorldTransform &transform,
               const SFT::Engine::SpotLightRenderer &light,
               SFT::Ecs::WriteResource<SFT::Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
        engine.render_extraction_schedule().add_system(
            [](SFT::Ecs::Entity entity,
               const SFT::Engine::WorldTransform &transform,
               const SFT::Engine::PointLightRenderer &light,
               SFT::Ecs::WriteResource<SFT::Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
    }

} // namespace SFT::Ffi

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_render_shape_params_init(SturdyShapeParams *params) {
    return guarded([&]() -> SturdyResult {
        if (params == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "params must not be null");
        }

        // Mirrors the defaults on the engine's own parameter structs, so a caller that initializes
        // and changes nothing gets exactly what C++ would.
        *params = SturdyShapeParams{};
        params->struct_size = static_cast<uint32_t>(sizeof(SturdyShapeParams));
        params->size = 1.0f;
        params->radius = 0.5f;
        params->height = 1.0f;
        params->width = 1.0f;
        params->depth = 1.0f;
        params->major_radius = 0.5f;
        params->minor_radius = 0.2f;
        params->extents_x = 1.0f;
        params->extents_y = 1.0f;
        params->extents_z = 1.0f;
        params->rings = 16;
        params->segments = 32;
        params->subdivisions = 2;
        params->radial_segments = 32;
        params->width_segments = 1;
        params->depth_segments = 1;
        params->major_segments = 32;
        params->minor_segments = 16;
        params->capped = STURDY_TRUE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_load_shader(SturdyEngine engine,
                                                       const char *source,
                                                       const char *depth_only_entry_point,
                                                       SturdyAsset *out_shader) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr || out_shader == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Engine::ShaderAssetDesc desc{};
        desc.source = std::string{source};
        desc.label = SFT::UString{source};
        if (depth_only_entry_point != nullptr) {
            desc.depth_only_fragment_entry_point = SFT::UString{depth_only_entry_point};
        }

        auto shader = resolved_engine->assets().load_shader(std::move(desc));
        if (!shader) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, shader.error().message.cpp_string_view());
        }
        *out_shader = to_abi_asset(*shader);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_create_shape_model(SturdyEngine engine,
                                                              SturdyShape shape,
                                                              const SturdyShapeParams *params,
                                                              SturdyAsset shader,
                                                              const char *label,
                                                              SturdyAsset *out_model) {
    return guarded([&]() -> SturdyResult {
        if (params == nullptr || out_model == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "params and output pointer must not be null");
        }
        if (params->struct_size != sizeof(SturdyShapeParams)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyShapeParams size does not match this engine build");
        }
        if (!all_finite({params->size, params->radius, params->height, params->width, params->depth,
                         params->major_radius, params->minor_radius, params->extents_x,
                         params->extents_y, params->extents_z})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "shape dimensions must be finite");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Renderer::Mesh mesh = SFT::Renderer::Mesh::create();
        const SturdyResult built = build_shape(shape, *params, label, &mesh);
        if (built != STURDY_OK) {
            return built;
        }

        auto model = resolved_engine->assets().create_model(
            std::move(mesh), to_engine_asset(shader), std::nullopt,
            label != nullptr ? SFT::UString{label} : SFT::UString{});
        if (!model) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, model.error().message.cpp_string_view());
        }
        *out_model = to_abi_asset(*model);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_create_mesh_model(SturdyEngine engine,
                                                             const SturdyVertex *vertices,
                                                             uint32_t vertex_count,
                                                             const uint32_t *indices,
                                                             uint32_t index_count,
                                                             SturdyAsset shader,
                                                             const char *label,
                                                             SturdyAsset *out_model) {
    return guarded([&]() -> SturdyResult {
        if (vertices == nullptr || indices == nullptr || out_model == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "vertices, indices and output pointer must not be null");
        }
        if (vertex_count == 0 || index_count == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a mesh needs at least one triangle");
        }
        if (index_count % 3 != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "index count must be a multiple of three");
        }

        // Checked here rather than left to the GPU: an out-of-range index reads past the vertex
        // buffer during draw, which surfaces as corruption or a device loss far from this call.
        for (uint32_t index = 0; index < index_count; ++index) {
            if (indices[index] >= vertex_count) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                                 "an index refers to a vertex beyond the end of the vertex array");
            }
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        std::vector<SFT::Renderer::GeometryVertex> geometry;
        geometry.reserve(vertex_count);
        for (uint32_t index = 0; index < vertex_count; ++index) {
            const SturdyVertex &source = vertices[index];
            geometry.push_back(SFT::Renderer::GeometryVertex{
                .position = glm::vec3{source.position[0], source.position[1], source.position[2]},
                .normal = glm::vec3{source.normal[0], source.normal[1], source.normal[2]},
                .uv = glm::vec2{source.uv[0], source.uv[1]},
                .color = glm::vec4{source.color[0], source.color[1], source.color[2], source.color[3]},
                .tangent = glm::vec4{source.tangent[0], source.tangent[1], source.tangent[2],
                                     source.tangent[3]},
            });
        }

        const std::vector<u32> index_buffer{indices, indices + index_count};
        SFT::Renderer::Mesh mesh = SFT::Renderer::Mesh::from_vertices(geometry, index_buffer, label);

        auto model = resolved_engine->assets().create_model(
            std::move(mesh), to_engine_asset(shader), std::nullopt,
            label != nullptr ? SFT::UString{label} : SFT::UString{});
        if (!model) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, model.error().message.cpp_string_view());
        }
        *out_model = to_abi_asset(*model);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_model_float(SturdyEngine engine,
                                                           SturdyAsset model,
                                                           uint32_t primitive,
                                                           const char *name,
                                                           float value) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "parameter name must not be null");
        }
        if (!all_finite({value})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "material values must be finite");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto applied = resolved_engine->assets().set_model_float(to_engine_asset(model), primitive,
                                                                 std::string_view{name}, value);
        if (!applied) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, applied.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_model_vec4(SturdyEngine engine,
                                                          SturdyAsset model,
                                                          uint32_t primitive,
                                                          const char *name,
                                                          float x,
                                                          float y,
                                                          float z,
                                                          float w) {
    return guarded([&]() -> SturdyResult {
        if (name == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "parameter name must not be null");
        }
        if (!all_finite({x, y, z, w})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "material values must be finite");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto applied = resolved_engine->assets().set_model_vec4(
            to_engine_asset(model), primitive, std::string_view{name}, glm::vec4{x, y, z, w});
        if (!applied) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, applied.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_model_texture(SturdyEngine engine,
                                                             SturdyAsset model,
                                                             uint32_t primitive,
                                                             const char *slot,
                                                             SturdyAsset texture) {
    return guarded([&]() -> SturdyResult {
        if (slot == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "texture slot must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto applied = resolved_engine->assets().set_model_texture(
            to_engine_asset(model), primitive, std::string_view{slot}, to_engine_asset(texture));
        if (!applied) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, applied.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_load_texture(SturdyEngine engine,
                                                        const char *source,
                                                        SturdyBool srgb,
                                                        SturdyAsset *out_texture) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr || out_texture == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Engine::TextureColorSpace color_space = srgb != STURDY_FALSE
                                                               ? SFT::Engine::TextureColorSpace::Srgb
                                                               : SFT::Engine::TextureColorSpace::Linear;
        auto texture = resolved_engine->assets().load_texture(std::string{source}, color_space);
        if (!texture) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, texture.error().message.cpp_string_view());
        }
        *out_texture = to_abi_asset(*texture);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_load_texture_from_memory(SturdyEngine engine,
                                                                    const uint8_t *encoded_bytes,
                                                                    size_t encoded_size,
                                                                    SturdyBool srgb,
                                                                    SturdyAsset *out_texture) {
    return guarded([&]() -> SturdyResult {
        if (encoded_bytes == nullptr || out_texture == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "encoded_bytes and output pointer must not be null");
        }
        if (encoded_size == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "encoded_size must be nonzero");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Engine::TextureColorSpace color_space = srgb != STURDY_FALSE
                                                               ? SFT::Engine::TextureColorSpace::Srgb
                                                               : SFT::Engine::TextureColorSpace::Linear;
        const std::span<const std::byte> encoded{reinterpret_cast<const std::byte *>(encoded_bytes),
                                                 encoded_size};
        auto texture = resolved_engine->assets().create_texture_from_encoded_bytes(encoded, color_space);
        if (!texture) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, texture.error().message.cpp_string_view());
        }
        *out_texture = to_abi_asset(*texture);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_texture_desc_init(SturdyRenderTextureDesc *desc) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::Engine::TextureAssetDesc defaults{};
        *desc = SturdyRenderTextureDesc{};
        desc->struct_size = static_cast<uint32_t>(sizeof(SturdyRenderTextureDesc));
        desc->width = defaults.width;
        desc->height = defaults.height;
        desc->srgb = defaults.color_space == SFT::Engine::TextureColorSpace::Srgb ? STURDY_TRUE : STURDY_FALSE;
        desc->allow_compression = defaults.allow_compression ? STURDY_TRUE : STURDY_FALSE;
        desc->generate_mipmaps = defaults.generate_mipmaps ? STURDY_TRUE : STURDY_FALSE;
        desc->rgba8 = nullptr;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_create_texture(SturdyEngine engine,
                                                          const SturdyRenderTextureDesc *desc,
                                                          SturdyAsset *out_texture) {
    return guarded([&]() -> SturdyResult {
        if (desc == nullptr || out_texture == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "desc and output pointer must not be null");
        }
        if (desc->struct_size != sizeof(SturdyRenderTextureDesc)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyRenderTextureDesc size does not match this engine build");
        }
        // Capped so `width * height * 4` cannot overflow while building the byte span below; no
        // real GPU's max_texture_dimension_2d comes anywhere close to this.
        constexpr uint32_t max_dimension = 65536;
        if (desc->width == 0 || desc->height == 0 || desc->width > max_dimension ||
            desc->height > max_dimension) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "width and height must be nonzero and no greater than 65536");
        }
        if (desc->rgba8 == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "rgba8 must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Engine::TextureAssetDesc engine_desc{};
        engine_desc.width = desc->width;
        engine_desc.height = desc->height;
        engine_desc.color_space = desc->srgb != STURDY_FALSE ? SFT::Engine::TextureColorSpace::Srgb
                                                              : SFT::Engine::TextureColorSpace::Linear;
        const size_t pixel_bytes = static_cast<size_t>(desc->width) * static_cast<size_t>(desc->height) * 4;
        const auto *pixels = reinterpret_cast<const std::byte *>(desc->rgba8);
        // The C ABI's SturdyTextureDesc is 8-bit by construction (its field is literally named
        // rgba8), so this stays TexturePixelFormat::Rgba8, which is the default. Exposing HDR
        // textures across the ABI needs a new descriptor field and is deliberately not smuggled in
        // by reinterpreting this one.
        engine_desc.pixels.assign(pixels, pixels + pixel_bytes);
        engine_desc.allow_compression = desc->allow_compression != STURDY_FALSE;
        engine_desc.generate_mipmaps = desc->generate_mipmaps != STURDY_FALSE;

        auto texture = resolved_engine->assets().create_texture(std::move(engine_desc));
        if (!texture) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, texture.error().message.cpp_string_view());
        }
        *out_texture = to_abi_asset(*texture);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_model_info(SturdyEngine engine,
                                                      SturdyAsset model,
                                                      uint32_t *out_primitives,
                                                      uint32_t *out_vertices,
                                                      uint32_t *out_triangles) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto info = resolved_engine->assets().model_info(to_engine_asset(model));
        if (!info) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, info.error().message.cpp_string_view());
        }
        if (out_primitives != nullptr) {
            *out_primitives = static_cast<uint32_t>(info->primitive_count);
        }
        if (out_vertices != nullptr) {
            *out_vertices = static_cast<uint32_t>(info->vertex_count);
        }
        if (out_triangles != nullptr) {
            *out_triangles = static_cast<uint32_t>(info->triangle_count);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_unload_asset(SturdyEngine engine, SturdyAsset asset) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto unloaded = resolved_engine->assets().unload(to_engine_asset(asset));
        if (!unloaded) {
            const SturdyResult code = unloaded.error().code == SFT::Engine::AssetErrorCode::InUse
                                          ? STURDY_ERROR_BUSY
                                          : STURDY_ERROR_INVALID_HANDLE;
            return set_error(code, unloaded.error().message.cpp_string_view());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_spawn(SturdyEngine engine, SturdyEntity *out_entity) {
    return guarded([&]() -> SturdyResult {
        if (out_entity == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // registry.component<T>() registers on first use, which is what makes the engine's own
        // components reachable by name afterwards. Doing it here means a caller never has to know
        // that they are lazily registered.
        SFT::Ecs::World &world = resolved_engine->ecs_world();
        const SFT::Ecs::ComponentId id = world.registry().component<SFT::Engine::WorldTransform>();

        const SFT::Engine::WorldTransform transform{};
        const SFT::Ecs::ComponentId ids[]{id};
        const void *const data[]{&transform};

        auto spawned = world.spawn_erased(ids, data);
        if (!spawned) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, spawned.error().message.cpp_string_view());
        }
        out_entity->index = spawned->index;
        out_entity->generation = spawned->generation;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_transform(SturdyEngine engine,
                                                         SturdyEntity entity,
                                                         const float *matrix) {
    return guarded([&]() -> SturdyResult {
        if (matrix == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "matrix must not be null");
        }
        for (int index = 0; index < 16; ++index) {
            if (!std::isfinite(matrix[index])) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "transform values must be finite");
            }
        }

        SFT::Engine::WorldTransform transform{};
        std::memcpy(&transform.value, matrix, sizeof(float) * 16);
        return set_component(engine, entity, transform);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_light_direction(SturdyEngine engine,
                                                               SturdyEntity entity,
                                                               float x,
                                                               float y,
                                                               float z) {
    return guarded([&]() -> SturdyResult {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "direction values must be finite");
        }

        const glm::vec3 direction{x, y, z};
        const float length = glm::length(direction);
        if (length < 1e-6f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "direction must not be zero-length");
        }
        const glm::vec3 forward = direction / length;

        // The engine reads a light's direction as its transform's local -Y axis, so the rotation's
        // second column is -forward. The other two columns only have to complete a right-handed
        // orthonormal basis; which way they point around the axis is unobservable for a light.
        //
        // Building the basis here rather than documenting the convention is deliberate: a caller
        // that writes the direction straight into a column of an otherwise-identity matrix gets a
        // non-orthonormal transform that silently lights the scene from the wrong direction, with
        // every call still reporting success.
        const glm::vec3 up = -forward;
        const glm::vec3 reference =
            std::abs(up.z) < 0.9f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
        const glm::vec3 right = glm::normalize(glm::cross(up, reference));
        const glm::vec3 back = glm::cross(right, up);

        SFT::Engine::WorldTransform transform{};
        transform.value = glm::mat4{1.0f};
        transform.value[0] = glm::vec4{right, 0.0f};
        transform.value[1] = glm::vec4{up, 0.0f};
        transform.value[2] = glm::vec4{back, 0.0f};
        return set_component(engine, entity, transform);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_get_transform(SturdyEngine engine,
                                                         SturdyEntity entity,
                                                         float *out_matrix) {
    return guarded([&]() -> SturdyResult {
        if (out_matrix == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Ecs::World &world = resolved_engine->ecs_world();
        const SFT::Ecs::ComponentId id = world.registry().component<SFT::Engine::WorldTransform>();
        SFT::Engine::WorldTransform transform{};
        auto read = world.read_component_erased(
            SFT::Ecs::Entity{.index = entity.index, .generation = entity.generation}, id, &transform,
            sizeof(transform));
        if (!read) {
            return set_error(read.error().code == SFT::Ecs::WorldErasedErrorCode::DeadEntity
                                 ? STURDY_ERROR_ENTITY_NOT_ALIVE
                                 : STURDY_ERROR_COMPONENT_MISSING,
                             read.error().message.cpp_string_view());
        }
        std::memcpy(out_matrix, &transform.value, sizeof(float) * 16);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_model(SturdyEngine engine,
                                                     SturdyEntity entity,
                                                     SturdyAsset model,
                                                     SturdyBool visible) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::ModelRenderer renderer{};
        renderer.model = to_engine_asset(model);
        renderer.visible = visible != STURDY_FALSE;
        return set_component(engine, entity, renderer);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_directional_light(SturdyEngine engine,
                                                                 SturdyEntity entity,
                                                                 const float *radiance,
                                                                 float angular_radius_degrees,
                                                                 SturdyBool casts_shadows) {
    return guarded([&]() -> SturdyResult {
        if (radiance == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be null");
        }
        if (!all_finite({radiance[0], radiance[1], radiance[2], angular_radius_degrees})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "light values must be finite");
        }
        if (radiance[0] < 0.0f || radiance[1] < 0.0f || radiance[2] < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be negative");
        }
        if (angular_radius_degrees <= 0.0f || angular_radius_degrees >= 90.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "angular radius must be within (0, 90) degrees");
        }

        SFT::Engine::DirectionalLightRenderer light{};
        light.radiance = glm::vec3{radiance[0], radiance[1], radiance[2]};
        light.angular_radius_degrees = angular_radius_degrees;
        light.casts_shadows = casts_shadows != STURDY_FALSE;
        return set_component(engine, entity, light);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_point_light(SturdyEngine engine,
                                                           SturdyEntity entity,
                                                           const float *radiance,
                                                           float range,
                                                           float source_radius,
                                                           SturdyBool casts_shadows) {
    return guarded([&]() -> SturdyResult {
        if (radiance == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be null");
        }
        if (!all_finite({radiance[0], radiance[1], radiance[2], range, source_radius})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "light values must be finite");
        }
        if (radiance[0] < 0.0f || radiance[1] < 0.0f || radiance[2] < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be negative");
        }
        if (range <= 0.0f || source_radius < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "range must be positive and source radius not negative");
        }

        SFT::Engine::PointLightRenderer light{};
        light.radiance = glm::vec3{radiance[0], radiance[1], radiance[2]};
        light.range = range;
        light.source_radius = source_radius;
        light.casts_shadows = casts_shadows != STURDY_FALSE;
        return set_component(engine, entity, light);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_render_set_spot_light(SturdyEngine engine,
                                                          SturdyEntity entity,
                                                          const float *radiance,
                                                          float range,
                                                          float inner_cone_degrees,
                                                          float outer_cone_degrees,
                                                          float source_radius,
                                                          SturdyBool casts_shadows) {
    return guarded([&]() -> SturdyResult {
        if (radiance == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be null");
        }
        if (!all_finite({radiance[0], radiance[1], radiance[2], range, inner_cone_degrees,
                         outer_cone_degrees, source_radius})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "light values must be finite");
        }
        if (radiance[0] < 0.0f || radiance[1] < 0.0f || radiance[2] < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "radiance must not be negative");
        }
        if (range <= 0.0f || source_radius < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "range must be positive and source radius not negative");
        }
        if (inner_cone_degrees < 0.0f || outer_cone_degrees >= 90.0f ||
            inner_cone_degrees > outer_cone_degrees) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "cone angles must satisfy 0 <= inner <= outer < 90 degrees");
        }

        // The engine stores cosines rather than angles, since that is what the shader compares
        // against; converting here keeps the ABI in the units a caller actually thinks in.
        constexpr float degrees_to_radians = 3.14159265358979323846f / 180.0f;
        SFT::Engine::SpotLightRenderer light{};
        light.radiance = glm::vec3{radiance[0], radiance[1], radiance[2]};
        light.range = range;
        light.inner_cone_cos = std::cos(inner_cone_degrees * degrees_to_radians);
        light.outer_cone_cos = std::cos(outer_cone_degrees * degrees_to_radians);
        light.source_radius = source_radius;
        light.casts_shadows = casts_shadows != STURDY_FALSE;
        return set_component(engine, entity, light);
    });
}

} // extern "C"
