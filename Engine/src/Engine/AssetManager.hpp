#pragma once

#include "Asset.hpp"

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

    /// High-level graphics-shader description. Backend target selection, compilation, reflection,
    /// pipeline layouts, hot reload, and destruction remain AssetManager/Renderer responsibilities.
    struct ShaderAssetDesc {
        std::filesystem::path source;
        UString label;
        UString module_name;
        UString vertex_entry_point{UString{"vertexMain"_ustr}};
        UString fragment_entry_point{UString{"fragmentMain"_ustr}};
        /// Optional second fragment entry point, compiled from the same source, for the material's Z
        /// prepass (alpha-tested cutout materials need it to sample base_color_texture/alpha_cutoff
        /// there too — see Renderer::depth_only_pipeline_for). Empty (the default) means the template
        /// has no depth-only fragment; its prepass pipeline is then a bare position-only depth write.
        UString depth_only_fragment_entry_point;
        std::vector<ShaderDefine> defines;
    };

    enum class TextureColorSpace : u8 {
        Linear,
        Srgb,
    };

    /// The "Compression Manager" policy input: what kind of data this texture holds, which decides
    /// which BC format create_texture compresses it into (Detail::choose_bc_format). Every kind
    /// still respects allow_compression/device support the same way — this only picks the format
    /// once compression is otherwise going to happen.
    enum class TextureKind : u8 {
        /// Full RGBA color where alpha carries real information (cutout/blend alpha, or unknown) —
        /// BC7. This is the default, matching every caller's behavior before TextureKind existed.
        ColorAlpha,
        /// Full RGB color where alpha is known to be irrelevant (e.g. a glTF material with
        /// alpha_mode OPAQUE) — BC1, half BC7's size. Sampling always reads back alpha = 1.0.
        ColorOpaque,
        /// A single-channel mask/data texture sampled via `.r` alone (e.g. a standalone occlusion
        /// map) — BC4, half BC7's size. Sampling reads back (r, 0, 0, 1).
        Mask,
        /// A two-channel tangent-space normal map (X/Y only, shader reconstructs Z) sampled via
        /// `.rg` alone — BC5, same size as BC7 but tuned for uncorrelated 2-channel data. Sampling
        /// reads back (r, g, 0, 1).
        NormalMap,
        /// A standalone glTF-style metallic-roughness texture (G = roughness, B = metallic, no
        /// occlusion partner to pack with — see AssetManager::create_orm_texture for the packed
        /// case) — BC5, re-encoding the G/B channels into the format's R/G. Sampling reads back
        /// (roughness, metallic, 0, 1) instead of glTF's own (_, roughness, metallic, _) layout, so
        /// GltfImport.cpp also sets the material's metallic_roughness_channels_rg flag to tell
        /// gbuffer_geometry.slang which channels to read.
        MetallicRoughness,
    };

    struct TextureAssetDesc {
        u32 width = 0;
        u32 height = 0;
        TextureColorSpace color_space = TextureColorSpace::Srgb;
        std::vector<std::byte> rgba8;
        UString label;
        /// Which BC format create_texture compresses this into when compression happens at all —
        /// see TextureKind's own doc comment. Defaults to ColorAlpha (BC7), i.e. every existing
        /// caller that doesn't set this is unaffected.
        TextureKind kind = TextureKind::ColorAlpha;
        /// True (default) lets create_texture BC-compress this texture for a real VRAM win, when
        /// the device supports it and the texture is at least 4x4 (see
        /// RHI::DeviceLimits::supports_bc_texture_compression, Detail::choose_bc_format). Set false
        /// for a texture that must stay byte-exact — e.g. a data LUT sampled as non-visual data
        /// rather than a color image, where BC's lossy artifacts would corrupt the values.
        bool allow_compression = true;
        /// Visual textures default to a complete mip chain. Disable for data textures whose shader
        /// requires exact base-level sampling rather than filtered lower-resolution representations.
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

    /// A model may contain any number of primitives. Each primitive receives its own internally-owned
    /// material instance; consumers select a shader asset and optional initial texture bindings without
    /// ever touching mesh/material/texture GPU handles.
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
        /// index_count / 3 — every model primitive is built from an indexed, TriangleList-drawn Mesh
        /// (see AssetManager::create_model), so this is exact, not an approximation.
        usize triangle_count = 0;
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
                                                        TextureKind kind = TextureKind::ColorAlpha,
                                                        UString label = {});

        /// Packs a standalone occlusion map (R channel) and a glTF-style metallic-roughness map
        /// (G = roughness, B = metallic) into one RGBA8 texture and uploads it as a single
        /// compressed GPU texture, instead of two — the "Texture Set" case this pass wires end to
        /// end (see GltfImport.cpp, which binds the result to both the occlusion_texture and
        /// metallic_roughness_texture material slots; no shader changes needed since each slot
        /// already only reads its own channels). Both inputs must be tightly packed
        /// width*height*4 RGBA8 buffers of identical dimensions (Detail::pack_orm_rgba8's own
        /// contract) — returns an InvalidDescription error otherwise, so callers should fall back
        /// to create_texture()-ing the two sources independently rather than call this speculatively.
        [[nodiscard]] AssetExpected<Asset> create_orm_texture(
            std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
            u32 width, u32 height, UString label = {});

        /// Asynchronous counterpart to load_texture(): returns an Asset immediately, valid and
        /// bindable right away (create_model's texture-binding validation needs no changes to accept
        /// it -- see TextureStreamer's own class doc comment for the mechanism), whose GPU pixel data
        /// streams in over the following frames instead of blocking this call until upload completes.
        /// Deliberately NOT implemented in terms of load_texture() or vice versa: this path's decode/
        /// upload failures surface as a Failed streaming state (queryable via texture_info() reporting
        /// zero dimensions, or by holding onto the StreamedTextureHandle -- not currently exposed
        /// through AssetManager -- and querying TextureStreamer directly), not as this call's own
        /// return value, since the failure (if any) isn't known until after this function returns.
        /// No in-memory path_cache dedup (unlike load_texture): a second streamed request for the same
        /// path starts a second independent load rather than reusing an in-flight or completed one.
        [[nodiscard]] AssetExpected<Asset> load_texture_streamed(const std::filesystem::path &source,
                                                                  TextureColorSpace color_space = TextureColorSpace::Srgb,
                                                                  TextureKind kind = TextureKind::ColorAlpha,
                                                                  UString label = {});
        /// Call once per frame (alongside, or instead of, calling it directly on your own
        /// TextureStreamer instance) so streamed textures requested via load_texture_streamed()
        /// actually transition Pending/Uploading -> Resident.
        void pump_texture_streaming();

        /// Decodes an already-in-memory encoded image (PNG/JPEG) and uploads it, for sources that
        /// aren't a standalone file on disk — e.g. a glTF .glb's embedded buffer-view images or a
        /// data: URI's decoded bytes. load_texture()'s file-based API can't reach these since it
        /// always reads its own bytes from `source`.
        [[nodiscard]] AssetExpected<Asset> create_texture_from_encoded_bytes(
            std::span<const std::byte> encoded,
            TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha,
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

        /// High-level material mutation. The primitive's material instance and reflected GPU state stay
        /// hidden; names are validated by the renderer's reflection-derived material layout.
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

        /// The raw RHI-facing handle behind a live texture asset — for a consumer that hands textures
        /// to a lower-level API expecting a Renderer::TextureHandle directly (e.g.
        /// UI::Context::image(), which stays Engine/AssetManager-agnostic by design; see
        /// Engine::UiImageCache). Every other AssetManager consumer goes through Asset instead, so
        /// reach for this only when the callee genuinely can't take an Asset.
        [[nodiscard]] AssetExpected<SFT::Renderer::TextureHandle> texture_handle(Asset asset) const;
        [[nodiscard]] AssetExpected<SoundAssetInfo> sound_info(Asset asset) const;

        /// File and decoded-sound storage is shared so returned data remains alive across later manager
        /// insertions. Unloading that exact asset invalidates the handle but does not invalidate a copy
        /// already held by a caller.
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
