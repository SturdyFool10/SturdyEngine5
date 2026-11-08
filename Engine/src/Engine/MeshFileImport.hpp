#pragma once

#include <Engine/GltfImport.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace SFT::Engine {

    /// Imports a PLY (ASCII or binary, any byte order) or an STL (binary or ASCII) mesh as one model with one instance.
    /// PLY per-vertex colours, normals and UVs are kept; STL has flat per-face normals. Both formats are Z-up in practice
    /// (Blender, CAD, scanners), so the instance carries a rotation to the engine's Y-up. `name` is only used in messages
    /// and as the model label.
    [[nodiscard]] AssetExpected<GltfImportResult> import_ply_bytes(AssetManager &assets, std::span<const std::byte> bytes,
                                                                   std::string_view name, Asset shader);
    [[nodiscard]] AssetExpected<GltfImportResult> import_stl_bytes(AssetManager &assets, std::span<const std::byte> bytes,
                                                                   std::string_view name, Asset shader);
    [[nodiscard]] AssetExpected<GltfImportResult> import_ply(AssetManager &assets, const std::filesystem::path &source,
                                                             Asset shader);
    [[nodiscard]] AssetExpected<GltfImportResult> import_stl(AssetManager &assets, const std::filesystem::path &source,
                                                             Asset shader);

} // namespace SFT::Engine
