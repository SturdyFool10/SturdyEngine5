#pragma once

#include <Engine/Asset.hpp>

#include <Animation/Morph.hpp>
#include <Audio/Source.hpp>
#include <Animation/Skin.hpp>
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


    /// The sample layout a texture asset's pixel data is in. Deliberately a much smaller set than
    /// the image decoder can produce: these are the two the GPU texture path actually supports,
    /// and anything a decoder hands back is converted into one of them before it gets here.
    enum class TexturePixelFormat : u8 {
        /// Four 8-bit unsigned-normalized channels. Every ordinary texture.
        Rgba8,
        /// Four IEEE binary16 channels holding scene-linear light. The only format that can carry
        /// an HDR source's highlights above display white; not block-compressible by this engine
        /// (that would need a BC6H encoder, which the bundled bc7enc does not provide), so it
        /// trades VRAM for range.
        Rgba16Float,
    };


    /// Whether a texture load should preserve the source file's high dynamic range.
    enum class TextureDynamicRange : u8 {
        /// Decode to 8-bit sRGB, clipping anything above display white. Correct for the
        /// overwhelming majority of textures — albedo, UI, masks — and the only option that can be
        /// block-compressed.
        Sdr,
        /// Decode to scene-linear half-float in the working primaries, keeping highlights and any
        /// wide-gamut color the source carried. What an environment map or an HDR photograph wants;
        /// costs 2x the VRAM of an uncompressed 8-bit texture and 8x a BC1-compressed one.
        Hdr,
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
        /// Layout of `pixels`. `color_space` still describes how the GPU should interpret the
        /// samples; an `Rgba16Float` texture is always scene-linear and so must pair with
        /// `TextureColorSpace::Linear`.
        TexturePixelFormat format = TexturePixelFormat::Rgba8;
        /// Tightly packed pixel data in `format`, `width * height * bytes-per-pixel` bytes long.
        std::vector<std::byte> pixels;
        UString label;


        TextureKind kind = TextureKind::ColorAlpha;


        bool allow_compression = true;


        bool generate_mipmaps = true;

        /// Point-sample the texture (crisp texels when magnified: pixel-art skins, sprites) instead of trilinear.
        /// Fixed at creation; to change it live, create the texture again and rebind it on the model.
        bool nearest_sampling = false;
    };

    struct TextureAssetInfo {
        u32 width = 0;
        u32 height = 0;
        TextureColorSpace color_space = TextureColorSpace::Srgb;
    };

    /// How much of a texture asset's final image its GPU texture currently shows. The asset handle (and the renderer
    /// texture behind it) stays the same through every step; only the pixels get better.
    enum class TextureDetail : u8 {
        /// Nothing decoded yet; the texture is a transparent 1x1 stand-in.
        Placeholder,
        /// A first low-detail image is showing (a JPEG XL's DC approximation, upscaled to full size), possibly with
        /// parts already replaced by full detail.
        Partial,
        /// The finished image, with its mip chain and compression.
        Complete,
        /// Decoding failed; the texture stays whatever it last was.
        Failed,
    };

    struct TextureProgress {
        TextureDetail detail = TextureDetail::Complete;
        /// Share (0..1) of the image that has been replaced by full detail since the first low-detail image appeared. 0
        /// for `Placeholder`, 1 for `Complete`.
        f32 refined_fraction = 1.0f;
        /// Size of the GPU texture right now (the full size once the first low-detail image is showing).
        u32 width = 0;
        u32 height = 0;
        /// Bumped whenever the texture's pixels, size or format changed: poll it to know when to redraw cached results.
        u64 version = 0;
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
        bool double_sided = false;
        /// Optional skin influences (one entry per mesh vertex); lets `create_skinned_instance` deform a copy.
        std::shared_ptr<const Animation::SkinWeights> skin;
        /// Optional blend shapes. A primitive with morph targets but no skin is posed with an identity joint.
        std::shared_ptr<const Animation::MorphTargetSet> morph;
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
        /// Loads texture, decoding it on the calling thread before returning. For images that are shown on screen prefer
        /// `load_texture_progressive` (no blocking, low-detail first) and `load_svg` (vector sources); this is for data a
        /// caller needs the moment it returns (HDR sources, textures a model binds right away).
        ///
        /// @param source Source value or resource.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param label `label` value used by the operation.
        /// @param dynamic_range Whether to preserve the source's high dynamic range.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> load_texture(const std::filesystem::path &source,
                                                        TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                        TextureKind kind = TextureKind::ColorAlpha,
                                                        UString label = {},
                                                        TextureDynamicRange dynamic_range = TextureDynamicRange::Sdr);


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


        /// Loads an image for display without ever blocking on it: returns at once with an asset whose texture is a
        /// transparent 1x1 stand-in, and decodes on a worker thread. A JPEG XL shows its low-resolution DC approximation
        /// as soon as that is decoded, then has the finished parts replaced by full detail while the rest of the file
        /// decodes, then gets its mip chain and block compression; every other format appears once, finished. The asset
        /// and the renderer texture handle never change, so UI images and materials that use it just start showing the
        /// better pixels. Progress is applied by `pump_texture_streaming` (the engine calls it every frame). Loading the
        /// same file again returns the same asset.
        ///
        /// @param source Image file.
        /// @param color_space How the texture's samples are interpreted.
        /// @param label Debug name; the file name when empty.
        /// @param nearest_sampling Point-sample the texture (pixel art) instead of trilinear.
        /// @param kind What the texture holds, which picks the block-compression format of the finished texture.
        [[nodiscard]] AssetExpected<Asset> load_texture_progressive(const std::filesystem::path &source,
                                                                    TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                                    UString label = {}, bool nearest_sampling = false,
                                                                    TextureKind kind = TextureKind::ColorAlpha);

        /// `load_texture_progressive` for an image the caller already holds in memory (a download, an embedded picture):
        /// `encoded` is the complete file (PNG, JPEG, JPEG XL, ...). Not shared between callers; each call is a new asset.
        [[nodiscard]] AssetExpected<Asset> create_texture_progressive_from_bytes(
            std::vector<std::byte> encoded, TextureColorSpace color_space = TextureColorSpace::Srgb, UString label = {},
            bool nearest_sampling = false, TextureKind kind = TextureKind::ColorAlpha);

        /// How far a texture asset's image has got; see `TextureProgress`. Valid for every texture asset (a texture made
        /// any other way is simply `Complete`).
        [[nodiscard]] AssetExpected<TextureProgress> texture_progress(Asset asset) const;

        /// Gives a texture asset new contents of any size and format; its handle, and everything that shows it, stay the
        /// same. Cancels a progressive load still in flight for it. A texture created as an SVG becomes a plain texture.
        [[nodiscard]] AssetResult replace_texture(Asset asset, TextureAssetDesc desc);

        /// Loads an image that may be animated (GIF, APNG, animated WebP, AVIF sequence, animated JXL). The handle is an
        /// ordinary texture; for an animation the manager changes what it shows as time passes (driven by
        /// `pump_texture_streaming`), honouring each frame's delay and the loop count. A still image is just a texture.
        [[nodiscard]] AssetExpected<Asset> load_texture_animated(const std::filesystem::path &source,
                                                                 TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                                 UString label = {});
        /// `load_texture_animated` for encoded bytes already in memory; `name` identifies the asset.
        [[nodiscard]] AssetExpected<Asset> create_texture_animated_from_bytes(std::string_view name,
                                                                              std::vector<std::byte> encoded,
                                                                              TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                                              UString label = {});
        /// Pauses or resumes an animated texture (a no-op for a still one).
        [[nodiscard]] AssetResult set_animation_playing(Asset asset, bool playing);
        /// Whether `asset` is a texture with more than one frame.
        [[nodiscard]] bool is_animated(Asset asset) const;

        /// Loads an SVG file as a texture asset: vector source, raster managed by the asset manager. The raster is made at
        /// `target_px` (the largest dimension, rounded up to a multiple of 4 so block compression applies) with a mip
        /// chain, so drawing it smaller stays clean. Asking again for the same file returns the same asset, and a larger
        /// `target_px` re-rasterises it in place (the asset never gets smaller by being asked for less; see
        /// `set_svg_size`).
        [[nodiscard]] AssetExpected<Asset> load_svg(const std::filesystem::path &source, f32 target_px,
                                                    UString label = {});

        /// [`load_svg`] for SVG text the caller holds (nothing is read from or written to the filesystem). `name` identifies
        /// the icon: asking again with the same name returns the same asset (the text of the first call is kept and used for
        /// every later re-rasterisation), and a larger `target_px` grows its raster in place.
        [[nodiscard]] AssetExpected<Asset> load_svg_data(std::string_view name, std::string_view svg, f32 target_px,
                                                         UString label = {});

        /// Sets an SVG texture asset's raster size exactly, growing or shrinking it in place. Use it when the sizes the
        /// UI draws it at changed for good (a UI-scale or anti-aliasing setting). A no-op when the size is unchanged.
        [[nodiscard]] AssetResult set_svg_size(Asset asset, f32 target_px);

        /// Kept for existing callers: now exactly `load_texture_progressive` (same asset contract, always 8-bit sRGB or
        /// linear LDR; use `load_texture` with `TextureDynamicRange::Hdr` for HDR sources). The asset is usable at once and
        /// the engine finishes it on its own; the placeholder is transparent, not magenta.
        ///
        /// @param source Source value or resource.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param label `label` value used by the operation.
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
        /// @param dynamic_range Whether to preserve the source's high dynamic range.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> create_texture_from_encoded_bytes(
            std::span<const std::byte> encoded,
            TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha,
            UString label = {},
            TextureDynamicRange dynamic_range = TextureDynamicRange::Sdr);

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

        /// Makes a file asset from bytes already in memory. `name` identifies it (asking again with the same name returns
        /// the same asset); nothing touches the filesystem.
        [[nodiscard]] AssetExpected<Asset> create_file_from_bytes(std::string_view name, std::vector<std::byte> bytes,
                                                                  UString label = {});
        /// Swaps a file asset's contents. The handle stays valid; readers holding the old bytes keep them.
        [[nodiscard]] AssetResult replace_file(Asset asset, std::vector<std::byte> bytes);
        /// Decodes encoded audio (WAV, FLAC, MP3, Ogg, ...) held in memory into a sound asset named `name`.
        /// `extension_hint` (like ".ogg", optional) breaks ties between codecs.
        [[nodiscard]] AssetExpected<Asset> create_sound_from_bytes(std::string_view name, std::vector<std::byte> encoded,
                                                                   std::string_view extension_hint = {}, UString label = {});
        /// Swaps a sound asset's audio. The handle stays valid; voices already playing finish on the old buffer.
        [[nodiscard]] AssetResult replace_sound(Asset asset, std::vector<std::byte> encoded,
                                                std::string_view extension_hint = {});
        /// Rebuilds a model asset from `desc` under the same handle (a re-import, an edited mesh, an LOD swap). The new
        /// model is built first, so on failure the old one is left as it was.
        [[nodiscard]] AssetResult replace_model(Asset asset, ModelAssetDesc desc);

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

        /// Clones a model whose primitives carry skin weights into a new model asset with its own vertex storage
        /// (materials are shared with the source), so each skinned character can be posed independently. The
        /// clone starts in bind pose. Unload it like any model; unloading the source first leaves the clone's
        /// materials dangling, so keep the source alive while clones exist.
        /// True when `create_skinned_instance` has something to deform: skin weights or blend shapes.
        [[nodiscard]] bool is_skinnable(Asset model) const;

        [[nodiscard]] AssetExpected<Asset> create_skinned_instance(Asset model, UString label = {});

        /// Deforms every skinned primitive of a `create_skinned_instance` model with `skin_matrices` (see
        /// `Animation::skin_matrices`) and uploads the result.
        /// `morph_weights` drive the primitives' blend shapes (one weight per target); pass none for default weights.
        [[nodiscard]] AssetResult set_skinned_pose(Asset model, std::span<const glm::mat4> skin_matrices,
                                                   std::span<const f32> morph_weights = {});
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
        /// The sound as a shareable buffer with its channel count, rate, cue points and loop region; what the audio
        /// engine plays (see `Engine::sound_buffer`). No copy of the samples is made.
        [[nodiscard]] AssetExpected<std::shared_ptr<const Audio::SampleBuffer>> sound_buffer(Asset asset) const;

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
