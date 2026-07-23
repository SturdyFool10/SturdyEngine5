#pragma once

#include "Asset.hpp"

#include <Renderer/Mesh.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {
    class Renderer;
    struct SceneRenderable;
}

namespace SFT::Engine {

    class RenderFrameRequests;

    struct ShaderDefine {
        UString name;
        UString value;
    };

    // High-level graphics-shader description. Backend target selection, compilation, reflection,
    // pipeline layouts, hot reload, and destruction remain AssetManager/Renderer responsibilities.
    struct ShaderAssetDesc {
        std::filesystem::path source;
        UString label;
        UString module_name;
        UString vertex_entry_point{UString{"vertexMain"_ustr}};
        UString fragment_entry_point{UString{"fragmentMain"_ustr}};
        // Optional second fragment entry point, compiled from the same source, for the material's Z
        // prepass (alpha-tested cutout materials need it to sample base_color_texture/alpha_cutoff
        // there too — see Renderer::depth_only_pipeline_for). Empty (the default) means the template
        // has no depth-only fragment; its prepass pipeline is then a bare position-only depth write.
        UString depth_only_fragment_entry_point;
        std::vector<ShaderDefine> defines;
    };

    enum class TextureColorSpace : u8 {
        Linear,
        Srgb,
    };

    struct TextureAssetDesc {
        u32 width = 0;
        u32 height = 0;
        TextureColorSpace color_space = TextureColorSpace::Srgb;
        std::vector<std::byte> rgba8;
        UString label;
    };

    struct TextureAssetInfo {
        u32 width = 0;
        u32 height = 0;
        TextureColorSpace color_space = TextureColorSpace::Srgb;
    };

    struct SoundAssetInfo {
        u32 channels = 0;
        u32 sample_rate = 0;
        u64 frame_count = 0;
        f64 duration_seconds = 0.0;
    };

    struct ModelTextureBinding {
        UString slot;
        Asset texture{};
    };

    // A model may contain any number of primitives. Each primitive receives its own internally-owned
    // material instance; consumers select a shader asset and optional initial texture bindings without
    // ever touching mesh/material/texture GPU handles.
    struct ModelPrimitiveDesc {
        SFT::Renderer::Mesh mesh;
        Asset shader{};
        std::optional<glm::vec4> vertex_color;
        std::vector<ModelTextureBinding> textures;
    };

    struct ModelAssetDesc {
        UString label;
        std::vector<ModelPrimitiveDesc> primitives;
    };

    struct ModelAssetInfo {
        usize primitive_count = 0;
        usize vertex_count = 0;
        usize index_count = 0;
    };

    class AssetManager {
      public:
        explicit AssetManager(SFT::Renderer::Renderer &renderer);
        ~AssetManager();

        AssetManager(const AssetManager &) = delete;
        AssetManager &operator=(const AssetManager &) = delete;
        AssetManager(AssetManager &&) = delete;
        AssetManager &operator=(AssetManager &&) = delete;

        [[nodiscard]] AssetExpected<Asset> load_shader(ShaderAssetDesc desc);
        [[nodiscard]] AssetExpected<Asset> load_shader(const std::filesystem::path &source,
                                                       UString label = {});

        [[nodiscard]] AssetExpected<Asset> create_texture(TextureAssetDesc desc);
        [[nodiscard]] AssetExpected<Asset> load_texture(const std::filesystem::path &source,
                                                        TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                        UString label = {});

        // Decodes an already-in-memory encoded image (PNG/JPEG) and uploads it, for sources that
        // aren't a standalone file on disk — e.g. a glTF .glb's embedded buffer-view images or a
        // data: URI's decoded bytes. load_texture()'s file-based API can't reach these since it
        // always reads its own bytes from `source`.
        [[nodiscard]] AssetExpected<Asset> create_texture_from_encoded_bytes(
            std::span<const std::byte> encoded,
            TextureColorSpace color_space = TextureColorSpace::Srgb,
            UString label = {});

        [[nodiscard]] AssetExpected<Asset> load_sound(const std::filesystem::path &source,
                                                      UString label = {});
        [[nodiscard]] AssetExpected<Asset> load_file(const std::filesystem::path &source,
                                                     UString label = {});

        [[nodiscard]] AssetExpected<Asset> create_model(ModelAssetDesc desc);
        [[nodiscard]] AssetExpected<Asset> create_model(SFT::Renderer::Mesh mesh,
                                                        Asset shader,
                                                        std::optional<glm::vec4> vertex_color = std::nullopt,
                                                        UString label = {});

        // High-level material mutation. The primitive's material instance and reflected GPU state stay
        // hidden; names are validated by the renderer's reflection-derived material layout.
        [[nodiscard]] AssetResult set_model_float(Asset model, usize primitive, std::string_view name, f32 value);
        [[nodiscard]] AssetResult set_model_vec4(Asset model, usize primitive, std::string_view name,
                                                 const glm::vec4 &value);
        [[nodiscard]] AssetResult set_model_texture(Asset model, usize primitive, std::string_view slot,
                                                    Asset texture);

        [[nodiscard]] bool contains(Asset asset) const noexcept;
        [[nodiscard]] usize size() const noexcept;
        [[nodiscard]] AssetExpected<AssetInfo> info(Asset asset) const;
        [[nodiscard]] AssetExpected<ModelAssetInfo> model_info(Asset asset) const;
        [[nodiscard]] AssetExpected<TextureAssetInfo> texture_info(Asset asset) const;
        [[nodiscard]] AssetExpected<SoundAssetInfo> sound_info(Asset asset) const;

        // File and decoded-sound storage is shared so returned data remains alive across later manager
        // insertions. Unloading that exact asset invalidates the handle but does not invalidate a copy
        // already held by a caller.
        [[nodiscard]] AssetExpected<std::shared_ptr<const std::vector<std::byte>>> file_bytes(Asset asset) const;
        [[nodiscard]] AssetExpected<std::shared_ptr<const std::vector<f32>>> sound_samples(Asset asset) const;

        [[nodiscard]] AssetResult unload(Asset asset);
        void clear() noexcept;

      private:
        friend class RenderFrameRequests;

        [[nodiscard]] bool append_model_renderables(
            Asset model,
            const glm::mat4 &world_transform,
            u64 stable_id,
            u32 visibility_mask,
            u32 sort_key,
            std::vector<SFT::Renderer::SceneRenderable> &destination) const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Engine
