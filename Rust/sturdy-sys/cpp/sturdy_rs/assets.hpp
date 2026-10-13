// Rust <-> SturdyEngine 5 shim for `SFT::Engine::AssetManager` and glTF import. Everything here
// flattens `std::expected`/exceptions into `ok`/`error`/`message` outcome structs so nothing but
// plain data ever crosses back into Rust; see `sturdy_rs::assets` in the cxx-generated header for
// their field layout.
#pragma once

#include <array>
#include <cstdint>

#include <rust/cxx.h>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::assets {

    enum class AssetKind : ::std::uint8_t;
    enum class AssetErrorCode : ::std::uint8_t;
    enum class TextureColorSpace : ::std::uint8_t;
    enum class TextureKind : ::std::uint8_t;
    enum class TextureDynamicRange : ::std::uint8_t;
    enum class TexturePixelFormat : ::std::uint8_t;
    enum class GltfLightKind : ::std::uint8_t;
    enum class ShapeKind : ::std::uint8_t;
    enum class ShapeAxis : ::std::uint8_t;
    struct ShapeParams;

    struct AssetHandle;
    struct AssetOutcome;
    struct StatusOutcome;
    struct AssetInfoOutcome;
    struct TextureInfoOutcome;
    struct ModelInfoOutcome;
    struct SoundInfoOutcome;
    struct ModelVertex;
    struct ModelTextureBinding;
    struct ModelPrimitiveInput;
    struct BytesOutcome;
    struct SamplesOutcome;
    struct RendererTextureHandleOutcome;
    struct GltfImportOutcome;

    AssetOutcome assets_load_texture(EngineView &engine,
                                     rust::Str source,
                                     TextureColorSpace color_space,
                                     TextureKind kind,
                                     rust::Str label,
                                     TextureDynamicRange dynamic_range);

    AssetOutcome assets_create_texture_from_bytes(EngineView &engine,
                                                  rust::Slice<const ::std::uint8_t> encoded,
                                                  TextureColorSpace color_space,
                                                  TextureKind kind,
                                                  rust::Str label,
                                                  TextureDynamicRange dynamic_range);

    AssetOutcome assets_load_texture_streamed(EngineView &engine,
                                              rust::Str source,
                                              TextureColorSpace color_space,
                                              TextureKind kind,
                                              rust::Str label);

    void assets_pump_texture_streaming(EngineView &engine) noexcept;

    AssetOutcome assets_create_texture(EngineView &engine,
                                       rust::Slice<const ::std::uint8_t> pixels,
                                       ::std::uint32_t width,
                                       ::std::uint32_t height,
                                       TexturePixelFormat format,
                                       TextureColorSpace color_space,
                                       TextureKind kind,
                                       rust::Str label,
                                       bool allow_compression,
                                       bool generate_mipmaps);

    AssetOutcome assets_create_orm_texture(EngineView &engine,
                                           rust::Slice<const ::std::uint8_t> occlusion_rgba8,
                                           rust::Slice<const ::std::uint8_t> metallic_roughness_rgba8,
                                           ::std::uint32_t width,
                                           ::std::uint32_t height,
                                           rust::Str label);

    AssetOutcome assets_load_shader(EngineView &engine, rust::Str source, rust::Str label);
    AssetOutcome assets_load_shader_desc(EngineView &engine, rust::Str source, rust::Str label, rust::Str module_name,
                                         rust::Str vertex_entry_point, rust::Str fragment_entry_point,
                                         rust::Str depth_only_fragment_entry_point,
                                         const rust::Vec<rust::String> &define_names,
                                         const rust::Vec<rust::String> &define_values);
    AssetOutcome assets_load_sound(EngineView &engine, rust::Str source, rust::Str label);
    AssetOutcome assets_load_file(EngineView &engine, rust::Str source, rust::Str label);

    bool assets_generate_shape(ShapeKind shape, const ShapeParams &params, rust::Vec<ModelVertex> &out_vertices,
                               rust::Vec<std::uint32_t> &out_indices);
    AssetOutcome assets_create_model(EngineView &engine, rust::Str label,
                                     rust::Slice<const ModelPrimitiveInput> primitives);

    StatusOutcome assets_set_model_float(EngineView &engine, AssetHandle model, ::std::uint64_t primitive,
                                         rust::Str name, float value);
    StatusOutcome assets_set_model_vec4(EngineView &engine, AssetHandle model, ::std::uint64_t primitive,
                                        rust::Str name, ::std::array<float, 4> value);
    StatusOutcome assets_set_model_texture(EngineView &engine, AssetHandle model, ::std::uint64_t primitive,
                                           rust::Str slot, AssetHandle texture);

    StatusOutcome assets_unload(EngineView &engine, AssetHandle asset);
    void assets_clear(EngineView &engine) noexcept;

    bool assets_contains(const EngineView &engine, AssetHandle asset) noexcept;
    ::std::uint64_t assets_size(const EngineView &engine) noexcept;
    AssetInfoOutcome assets_info(const EngineView &engine, AssetHandle asset);
    TextureInfoOutcome assets_texture_info(const EngineView &engine, AssetHandle asset);
    ModelInfoOutcome assets_model_info(const EngineView &engine, AssetHandle asset);
    SoundInfoOutcome assets_sound_info(const EngineView &engine, AssetHandle asset);

    RendererTextureHandleOutcome assets_texture_handle(const EngineView &engine, AssetHandle asset);
    rust::String assets_update_texture(EngineView &engine, AssetHandle asset, rust::Slice<const std::uint8_t> pixels);
    BytesOutcome assets_file_bytes(const EngineView &engine, AssetHandle asset);
    SamplesOutcome assets_sound_samples(const EngineView &engine, AssetHandle asset);

    GltfImportOutcome gltf_import(EngineView &engine, rust::Str source, AssetHandle shader);

} // namespace sturdy_rs::assets
