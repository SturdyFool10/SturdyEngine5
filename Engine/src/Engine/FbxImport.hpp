#pragma once

#include <Engine/GltfImport.hpp>

#include <filesystem>

namespace SFT::Engine {

    /// Imports an FBX (or Wavefront OBJ) file with ufbx: meshes split per material, skins, blend shapes, PBR-ish
    /// materials with their textures, and every animation stack baked to 30 Hz linear tracks. The scene is converted
    /// to the engine's conventions (right-handed, Y up, metres). The result has the same shape as a glTF import, so
    /// `spawn_imported` handles both. Skinned meshes share one skin (index 0) covering the whole node hierarchy.
    ///
    /// With `animations_only` no meshes, materials or textures are created (and `assets` is not touched), which is how
    /// animation-only files (Mixamo/mocap exports) are loaded to be retargeted onto another character.
    [[nodiscard]] AssetExpected<GltfImportResult> import_fbx(AssetManager &assets, const std::filesystem::path &source,
                                                             Asset shader, bool animations_only = false);

} // namespace SFT::Engine
