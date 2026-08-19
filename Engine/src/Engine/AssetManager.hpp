#pragma once

#include <Engine/Asset.hpp>

#include <Renderer/Handles.hpp>
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


    struct ShaderAssetDesc {
        std::filesystem::path source;
        UString label;
        UString module_name;
        UString vertex_entry_point{UString{"vertexMain"_ustr}};
        UString fragment_entry_point{UString{"fragmentMain"_ustr}};


        UString depth_only_fragment_entry_point;
        std::vector<ShaderDefine> defines;
    };

    enum class TextureColorSpace : u8 {
        Linear,
        Srgb,
    };


    enum class TextureKind : u8 {


        ColorAlpha,


        ColorOpaque,


        Mask,


        NormalMap,


        MetallicRoughness,
    };

    struct TextureAssetDesc {
        u32 width = 0;
        u32 height = 0;
        TextureColorSpace color_space = TextureColorSpace::Srgb;
        std::vector<std::byte> rgba8;
        UString label;


        TextureKind kind = TextureKind::ColorAlpha;


        bool allow_compression = true;


        bool generate_mipmaps = true;
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


        usize triangle_count = 0;
    };

    class AssetManager {
      public:
        /// Constructs a `AssetManager` from the supplied initialization values.
        ///
        /// @param renderer Renderer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit AssetManager(SFT::Renderer::Renderer &renderer);
        /// Destroys the `AssetManager` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~AssetManager();

        /// Disables this construction form for `AssetManager`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        AssetManager(const AssetManager &) = delete;
        /// Assigns a new value to this `AssetManager`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        AssetManager &operator=(const AssetManager &) = delete;
        /// Disables this construction form for `AssetManager`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        AssetManager(AssetManager &&) = delete;
        /// Assigns a new value to this `AssetManager`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        AssetManager &operator=(AssetManager &&) = delete;

        /// Loads shader.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::NotFound`.
        [[nodiscard]] AssetExpected<Asset> load_shader(ShaderAssetDesc desc);
        /// Loads shader.
        ///
        /// @param source Source value or resource.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> load_shader(const std::filesystem::path &source,
                                                       UString label = {});

        /// Creates a texture from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`.
        [[nodiscard]] AssetExpected<Asset> create_texture(TextureAssetDesc desc);
        /// Loads texture.
        ///
        /// @param source Source value or resource.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> load_texture(const std::filesystem::path &source,
                                                        TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                        TextureKind kind = TextureKind::ColorAlpha,
                                                        UString label = {});


        /// Creates a orm texture from the supplied parameters.
        ///
        /// @param occlusion_rgba8 `occlusion_rgba8` value used by the operation.
        /// @param metallic_roughness_rgba8 `metallic_roughness_rgba8` value used by the operation.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`.
        [[nodiscard]] AssetExpected<Asset> create_orm_texture(
            std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
            u32 width, u32 height, UString label = {});


        /// Loads texture streamed.
        ///
        /// @param source Source value or resource.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::IoFailure`.
        [[nodiscard]] AssetExpected<Asset> load_texture_streamed(const std::filesystem::path &source,
                                                                  TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                                  TextureKind kind = TextureKind::ColorAlpha,
                                                                  UString label = {});


        /// Pumps texture streaming using the supplied arguments and current state.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void pump_texture_streaming();


        /// Computes the create texture from encoded bytes required by the supplied values.
        ///
        /// @param encoded `encoded` value used by the operation.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> create_texture_from_encoded_bytes(
            std::span<const std::byte> encoded,
            TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha,
            UString label = {});

        /// Loads sound.
        ///
        /// @param source Source value or resource.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
        [[nodiscard]] AssetExpected<Asset> load_sound(const std::filesystem::path &source,
                                                      UString label = {});
        /// Loads file.
        ///
        /// @param source Source value or resource.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> load_file(const std::filesystem::path &source,
                                                     UString label = {});

        /// Creates a model from the supplied parameters.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<Asset> create_model(ModelAssetDesc desc);
        /// Creates a model from the supplied parameters.
        ///
        /// @param mesh `mesh` value used by the operation.
        /// @param shader Shader used or affected by the operation.
        /// @param vertex_color `vertex_color` value used by the operation.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> create_model(SFT::Renderer::Mesh mesh,
                                                        Asset shader,
                                                        std::optional<glm::vec4> vertex_color = std::nullopt,
                                                        UString label = {});


        /// Sets the model float for this `AssetManager`.
        ///
        /// @param model `model` value used by the operation.
        /// @param primitive `primitive` value used by the operation.
        /// @param name Name used to identify or label the target.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
        [[nodiscard]] AssetResult set_model_float(Asset model, usize primitive, std::string_view name, f32 value);
        /// Sets the model vec4 for this `AssetManager`.
        ///
        /// @param model `model` value used by the operation.
        /// @param primitive `primitive` value used by the operation.
        /// @param name Name used to identify or label the target.
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
        [[nodiscard]] AssetResult set_model_vec4(Asset model, usize primitive, std::string_view name,
                                                 const glm::vec4 &value);
        /// Sets the model texture for this `AssetManager`.
        ///
        /// @param model `model` value used by the operation.
        /// @param primitive `primitive` value used by the operation.
        /// @param slot Binding or storage slot addressed by the operation.
        /// @param texture Texture used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
        [[nodiscard]] AssetResult set_model_texture(Asset model, usize primitive, std::string_view slot,
                                                    Asset texture);

        /// Reports whether contains holds for this `AssetManager`.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains(Asset asset) const noexcept;
        /// Returns the size for this `AssetManager`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        /// Returns the size for this `AssetManager`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;
        /// Performs the info operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<AssetInfo> info(Asset asset) const;
        /// Performs the model info operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<ModelAssetInfo> model_info(Asset asset) const;
        /// Performs the texture info operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<TextureAssetInfo> texture_info(Asset asset) const;


        /// Returns the texture handle associated with this `AssetManager`.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<SFT::Renderer::TextureHandle> texture_handle(Asset asset) const;
        /// Performs the sound info operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<SoundAssetInfo> sound_info(Asset asset) const;


        /// Computes the file bytes required by the supplied values.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<std::shared_ptr<const std::vector<std::byte>>> file_bytes(Asset asset) const;
        /// Performs the sound samples operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
        [[nodiscard]] AssetExpected<std::shared_ptr<const std::vector<f32>>> sound_samples(Asset asset) const;

        /// Performs the unload operation for `AssetManager` using the supplied arguments.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InUse`.
        [[nodiscard]] AssetResult unload(Asset asset);
        /// Clears the stored state or contents.
        ///
        /// @note This function does not throw exceptions.
        void clear() noexcept;

      private:
        friend class RenderFrameRequests;

        /// Appends the supplied value or range to the current contents.
        ///
        /// @param model `model` value used by the operation.
        /// @param world_transform World used or affected by the operation.
        /// @param stable_id Identifier of the target object or resource.
        /// @param visibility_mask `visibility_mask` value used by the operation.
        /// @param sort_key Key used to identify the requested entry.
        /// @param destination Destination value or resource.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
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
