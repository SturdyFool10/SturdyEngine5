/// C ABI implementation of glTF import.
///
/// `import_gltf` returns a `GltfImportResult` by value: vectors of assets, node instances and
/// lights. That has to survive the call for a foreign caller to walk it, so this layer owns the
/// result and hands back a token for it.
///
/// The scene handle is the ABI's only **owned** handle. Every other one — engine, frame, commands —
/// is scope-bound and dies when the callback that produced it returns, which is what makes stashing
/// them harmless. A scene instead lives until `sturdy_gltf_release`, because an importer's whole
/// point is to inspect the result long after the import call. It still goes through the same
/// never-reused token table, so releasing twice is reported rather than freeing someone else's
/// scene.
///
/// Model assets are deliberately *not* freed on release: they belong to the asset manager, and
/// anything already spawned is still drawing them.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;
    using SFT::u64;

    /// Scenes this ABI owns, keyed by the token handed to the caller.
    ///
    /// A `unique_ptr` so the address stays put while the map rehashes — the handle table stores a
    /// raw pointer to the result, and a moved-from element would leave it dangling.
    std::mutex g_scene_mutex;
    std::map<u64, std::unique_ptr<SFT::Engine::GltfImportResult>> g_scenes;

    /// Resolves a scene handle to the import result it refers to.
    ///
    /// @param scene Handle produced by `sturdy_gltf_import`.
    /// @param out_result Receives the borrowed result on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_scene(SturdyGltfScene scene,
                                             SFT::Engine::GltfImportResult **out_result) noexcept {
        void *pointer = nullptr;
        const SturdyResult result = resolve_handle(scene.token, HandleKind::GltfScene, &pointer);
        if (result != STURDY_OK) {
            return result;
        }
        *out_result = static_cast<SFT::Engine::GltfImportResult *>(pointer);
        return STURDY_OK;
    }

    /// Copies an engine asset into its opaque ABI representation.
    ///
    /// @param asset Engine-side asset.
    ///
    /// @return The opaque value.
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
    /// @return The engine-side asset.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SFT::Engine::Asset to_engine_asset(const SturdyAsset &asset) noexcept {
        SFT::Engine::Asset result{};
        std::memcpy(&result, &asset, sizeof(result));
        return result;
    }

    /// Converts a cone cosine to degrees for reporting.
    ///
    /// The engine stores cosines because that is what the shader compares against; the ABI reports
    /// degrees because that is what a caller reasons in. Clamped before `acos` so a value that has
    /// drifted a hair outside [-1, 1] through float math yields an angle rather than NaN.
    ///
    /// @param cosine Cone cosine.
    ///
    /// @return The half-angle in degrees.
    /// @note This function does not throw exceptions.
    [[nodiscard]] float cone_degrees(float cosine) noexcept {
        const float clamped = cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
        constexpr float radians_to_degrees = 180.0f / 3.14159265358979323846f;
        return std::acos(clamped) * radians_to_degrees;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_gltf_import(SturdyEngine engine,
                                                const char *source,
                                                SturdyAsset shader,
                                                SturdyGltfScene *out_scene) {
    return guarded([&]() -> SturdyResult {
        if (source == nullptr || out_scene == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "source and output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        auto imported = SFT::Engine::import_gltf(resolved_engine->assets(), std::string{source},
                                                 to_engine_asset(shader));
        if (!imported) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, imported.error().message.cpp_string_view());
        }

        auto owned = std::make_unique<SFT::Engine::GltfImportResult>(std::move(*imported));
        void *pointer = owned.get();
        const u64 token = mint_handle(HandleKind::GltfScene, pointer);
        {
            const std::lock_guard<std::mutex> lock{g_scene_mutex};
            g_scenes.emplace(token, std::move(owned));
        }
        out_scene->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_release(SturdyGltfScene scene) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        // Revoked before the storage is freed, so a concurrent resolve on another thread sees an
        // expired handle rather than a pointer that is about to become invalid.
        revoke_handle(scene.token);
        const std::lock_guard<std::mutex> lock{g_scene_mutex};
        g_scenes.erase(scene.token);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_model_count(SturdyGltfScene scene, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(result->models.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_model_at(SturdyGltfScene scene,
                                                  uint32_t index,
                                                  SturdyAsset *out_model) {
    return guarded([&]() -> SturdyResult {
        if (out_model == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->models.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "model index is out of range");
        }
        *out_model = to_abi_asset(result->models[index]);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_count(SturdyGltfScene scene, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(result->instances.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_at(SturdyGltfScene scene,
                                                     uint32_t index,
                                                     SturdyGltfInstance *out_instance) {
    return guarded([&]() -> SturdyResult {
        if (out_instance == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->instances.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "instance index is out of range");
        }

        const SFT::Engine::GltfNodeInstance &instance = result->instances[index];
        *out_instance = SturdyGltfInstance{};
        out_instance->struct_size = static_cast<uint32_t>(sizeof(SturdyGltfInstance));
        out_instance->model = to_abi_asset(instance.model);
        std::memcpy(out_instance->world_transform, &instance.world_transform, sizeof(float) * 16);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_instance_name(SturdyGltfScene scene,
                                                       uint32_t index,
                                                       char *buffer,
                                                       size_t capacity,
                                                       size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->instances.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "instance index is out of range");
        }
        return copy_string_out(result->instances[index].name.cpp_string_view(), buffer, capacity,
                               out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_light_count(SturdyGltfScene scene, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_count = static_cast<uint32_t>(result->lights.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_light_at(SturdyGltfScene scene,
                                                  uint32_t index,
                                                  SturdyGltfLight *out_light) {
    return guarded([&]() -> SturdyResult {
        if (out_light == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->lights.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "light index is out of range");
        }

        const SFT::Engine::GltfLightInstance &light = result->lights[index];
        *out_light = SturdyGltfLight{};
        out_light->struct_size = static_cast<uint32_t>(sizeof(SturdyGltfLight));
        switch (light.kind) {
        case SFT::Engine::GltfLightKind::Directional:
            out_light->kind = STURDY_GLTF_LIGHT_DIRECTIONAL;
            break;
        case SFT::Engine::GltfLightKind::Spot:
            out_light->kind = STURDY_GLTF_LIGHT_SPOT;
            break;
        case SFT::Engine::GltfLightKind::Point:
        default:
            out_light->kind = STURDY_GLTF_LIGHT_POINT;
            break;
        }
        out_light->radiance[0] = light.radiance.x;
        out_light->radiance[1] = light.radiance.y;
        out_light->radiance[2] = light.radiance.z;
        out_light->range = light.range;
        out_light->inner_cone_degrees = cone_degrees(light.inner_cone_cos);
        out_light->outer_cone_degrees = cone_degrees(light.outer_cone_cos);
        std::memcpy(out_light->world_transform, &light.world_transform, sizeof(float) * 16);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_light_name(SturdyGltfScene scene,
                                                    uint32_t index,
                                                    char *buffer,
                                                    size_t capacity,
                                                    size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult resolved = resolve_scene(scene, &result);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (index >= result->lights.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "light index is out of range");
        }
        return copy_string_out(result->lights[index].name.cpp_string_view(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gltf_spawn_all(SturdyEngine engine,
                                                   SturdyGltfScene scene,
                                                   uint32_t *out_spawned) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Engine::GltfImportResult *result = nullptr;
        const SturdyResult scene_resolved = resolve_scene(scene, &result);
        if (scene_resolved != STURDY_OK) {
            return scene_resolved;
        }

        SFT::Ecs::World &world = resolved_engine->ecs_world();
        uint32_t spawned = 0;

        for (const SFT::Engine::GltfNodeInstance &instance : result->instances) {
            // A glTF node without a mesh still exists in the hierarchy; spawning it would create an
            // entity that neither draws nor lights anything.
            if (!instance.model) {
                continue;
            }
            (void)world.spawn(SFT::Engine::WorldTransform{.value = instance.world_transform},
                              SFT::Engine::ModelRenderer{.model = instance.model});
            ++spawned;
        }

        for (const SFT::Engine::GltfLightInstance &light : result->lights) {
            const SFT::Engine::WorldTransform transform{.value = light.world_transform};
            // Cone cosines are passed straight through rather than round-tripped through degrees,
            // so a spot light spawned here is bit-identical to what the importer produced.
            switch (light.kind) {
            case SFT::Engine::GltfLightKind::Directional:
                (void)world.spawn(transform,
                                  SFT::Engine::DirectionalLightRenderer{.radiance = light.radiance});
                break;
            case SFT::Engine::GltfLightKind::Spot:
                (void)world.spawn(transform, SFT::Engine::SpotLightRenderer{
                                                 .radiance = light.radiance,
                                                 .range = light.range,
                                                 .inner_cone_cos = light.inner_cone_cos,
                                                 .outer_cone_cos = light.outer_cone_cos,
                                             });
                break;
            case SFT::Engine::GltfLightKind::Point:
            default:
                (void)world.spawn(SFT::Engine::WorldTransform{transform},
                                  SFT::Engine::PointLightRenderer{.radiance = light.radiance,
                                                                  .range = light.range});
                break;
            }
            ++spawned;
        }

        if (out_spawned != nullptr) {
            *out_spawned = spawned;
        }
        return STURDY_OK;
    });
}

} // extern "C"
