#pragma once

#include "Asset.hpp"

#include <filesystem>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace SFT::Engine {

    class AssetManager;


    struct GltfNodeInstance {
        UString name;
        Asset model{};
        glm::mat4 world_transform{1.0f};
    };

    enum class GltfLightKind : u8 {
        Directional,
        Point,
        Spot,
    };


    struct GltfLightInstance {
        UString name;
        GltfLightKind kind = GltfLightKind::Point;
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 inner_cone_cos = 0.97f;
        f32 outer_cone_cos = 0.90f;
        glm::mat4 world_transform{1.0f};
    };

    struct GltfImportResult {


        std::vector<Asset> models;
        std::vector<GltfNodeInstance> instances;
        std::vector<GltfLightInstance> lights;
    };


    /// Imports gltf using the supplied arguments and current state.
    ///
    /// @param assets `assets` value used by the operation.
    /// @param source Source value or resource.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] AssetExpected<GltfImportResult> import_gltf(
        AssetManager &assets,
        const std::filesystem::path &source,
        Asset shader);

} // namespace SFT::Engine
