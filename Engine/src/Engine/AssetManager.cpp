#include <Engine/AssetManager.hpp>
#include <Engine/ImageDecode.hpp>
#include <Engine/TextureCompression.hpp>
#include <Engine/TextureMipChain.hpp>
#include <Engine/TextureStreamer.hpp>

#include <Core/Core.hpp>
#include <Core/Slang/EmbeddedShaders.hpp>
#include <Renderer/Renderer.hpp>
#include <RHI/RHI.hpp>

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace SFT::Engine {

    namespace {

        std::atomic<u64> next_asset_manager_id{1};

        /// Performs the path label operation for `Engine` using the supplied arguments.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString path_label(const std::filesystem::path &path, std::string_view fallback) {
            const std::string name = path.filename().string();
            if (auto utf8 = UString::try_from_utf8(name)) {
                return std::move(*utf8);
            }
            return UString{fallback};
        }

        /// Rewrites text that is not valid UTF-8 into text that is, preserving what it can.
        ///
        /// Bytes that cannot begin or continue a valid sequence become `?`. Used only on failure
        /// paths, where a diagnostic from a backend or shader compiler may carry raw bytes from an
        /// OS or driver string, and where dropping the message entirely would be worse than
        /// showing it with a few characters replaced.
        ///
        /// @param message Possibly-invalid UTF-8.
        ///
        /// @return A valid UTF-8 rendering of `message`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString sanitize_to_utf8(std::string_view message) {
            std::string sanitized;
            sanitized.reserve(message.size());
            for (usize index = 0; index < message.size();) {
                // Longest valid sequence starting here wins; anything else is one replaced byte.
                usize length = 0;
                const auto lead = static_cast<unsigned char>(message[index]);
                if (lead < 0x80u) {
                    length = 1;
                } else if ((lead & 0xE0u) == 0xC0u) {
                    length = 2;
                } else if ((lead & 0xF0u) == 0xE0u) {
                    length = 3;
                } else if ((lead & 0xF8u) == 0xF0u) {
                    length = 4;
                }

                bool valid = length != 0 && index + length <= message.size();
                for (usize offset = 1; valid && offset < length; ++offset) {
                    valid = (static_cast<unsigned char>(message[index + offset]) & 0xC0u) == 0x80u;
                }
                if (valid && UString::try_from_utf8(message.substr(index, length))) {
                    sanitized.append(message.substr(index, length));
                    index += length;
                } else {
                    sanitized.push_back('?');
                    ++index;
                }
            }

            auto text = UString::try_from_utf8(sanitized);
            return text ? std::move(*text) : UString{"(unprintable error message)"};
        }

        [[nodiscard]] AssetError error(AssetErrorCode code,
                                       std::string message,
                                       std::filesystem::path source = {}) {
            // try_from_utf8 rather than UString{message}: the validating constructor throws on
            // invalid UTF-8, and this runs on the failure path, where a throw has nowhere to go —
            // a backend message carrying a stray byte would terminate the process instead of
            // reporting the failure it was describing. Backend and shader-compiler diagnostics are
            // exactly the strings most likely to contain one.
            //
            // Sanitized rather than discarded when that happens: the message is the whole reason
            // this error exists, so losing it to one bad byte would trade a crash for a mystery.
            auto text = UString::try_from_utf8(message);
            return AssetError{
                .code = code,
                .message = text ? std::move(*text) : sanitize_to_utf8(message),
                .source = std::move(source),
            };
        }

        [[nodiscard]] AssetError backend_error(const Core::GraphicsBackendError &failure,
                                               const std::filesystem::path &source = {}) {
            return error(AssetErrorCode::BackendFailure, failure.message, source);
        }

        /// Reads binary file from the associated source.
        ///
        /// @param source Source value or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::IoFailure`, `AssetErrorCode::NotFound`.
        [[nodiscard]] AssetExpected<std::vector<std::byte>> read_binary_file(
            const std::filesystem::path &source) {
            std::ifstream file(source, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                const AssetErrorCode code = std::filesystem::exists(source)
                                                ? AssetErrorCode::IoFailure
                                                : AssetErrorCode::NotFound;
                return std::unexpected(error(code, "Could not open asset file '" + source.string() + "'.", source));
            }

            const std::streamoff end = file.tellg();
            if (end < 0) {
                return std::unexpected(error(
                    AssetErrorCode::IoFailure,
                    "Could not determine the size of asset file '" + source.string() + "'.",
                    source));
            }
            if (static_cast<u64>(end) > std::numeric_limits<usize>::max()) {
                return std::unexpected(error(
                    AssetErrorCode::IoFailure,
                    "Asset file is too large to address in this process: '" + source.string() + "'.",
                    source));
            }

            std::vector<std::byte> bytes(static_cast<usize>(end));
            file.seekg(0, std::ios::beg);
            if (!bytes.empty() &&
                !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
                return std::unexpected(error(
                    AssetErrorCode::IoFailure,
                    "Could not read the complete asset file '" + source.string() + "'.",
                    source));
            }
            return bytes;
        }

        /// Performs the combine stable ID operation for `Engine` using the supplied arguments.
        ///
        /// @param object `object` value used by the operation.
        /// @param primitive `primitive` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 combine_stable_id(u64 object, usize primitive) noexcept {


            const u64 value = static_cast<u64>(primitive) + 0x9e3779b97f4a7c15ULL;
            return object ^ (value + (object << 6u) + (object >> 2u));
        }

    } // namespace

    struct AssetManager::Impl {
        struct ShaderData {
            SFT::Renderer::MaterialTemplateHandle material_template{};
        };

        struct TextureData {
            SFT::Renderer::TextureHandle texture{};
            TextureAssetInfo info{};
        };

        struct SoundData {
            std::shared_ptr<std::vector<f32>> samples;
            SoundAssetInfo info{};
        };

        struct FileData {
            std::shared_ptr<std::vector<std::byte>> bytes;
        };

        struct ModelPrimitiveData {
            SFT::Renderer::MeshHandle mesh{};
            SFT::Renderer::MaterialInstanceHandle material{};
            Asset shader{};
            std::vector<Asset> textures;
            bool double_sided = false;
        };

        struct ModelData {
            std::vector<ModelPrimitiveData> primitives;
            ModelAssetInfo info{};
        };

        using Data = std::variant<std::monostate, ModelData, ShaderData, SoundData, TextureData, FileData>;

        struct Record {
            AssetType type = AssetType::Invalid;
            u32 generation = 1;
            bool loaded = false;
            UString label;
            std::filesystem::path source;
            usize memory_bytes = 0;
            Data data;
        };

        /// Performs the impl operation for `AssetManager` using the supplied arguments.
        ///
        /// @param renderer_ref Renderer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit Impl(SFT::Renderer::Renderer &renderer_ref)
            : renderer(renderer_ref), owner(next_asset_manager_id.fetch_add(1, std::memory_order_relaxed)),
              texture_streamer(renderer_ref) {
            if (owner == 0) {
                owner = next_asset_manager_id.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// Resolves the asset associated with the supplied key, handle, or resource.
        ///
        /// @param index Zero-based index of the target element or entry.
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Asset asset_for(usize index, const Record &record) const noexcept {
            return Asset{owner, static_cast<u64>(index + 1), record.generation, record.type};
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Record *find(Asset asset) noexcept {
            if (!asset || asset.owner_ != owner || asset.id_ > records.size()) {
                return nullptr;
            }
            Record &record = records[static_cast<usize>(asset.id_ - 1)];
            if (!record.loaded || record.generation != asset.generation_ || record.type != asset.type_) {
                return nullptr;
            }
            return &record;
        }

        /// Finds the requested entry in the available state.
        ///
        /// @param asset `asset` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Record *find(Asset asset) const noexcept {
            if (!asset || asset.owner_ != owner || asset.id_ > records.size()) {
                return nullptr;
            }
            const Record &record = records[static_cast<usize>(asset.id_ - 1)];
            if (!record.loaded || record.generation != asset.generation_ || record.type != asset.type_) {
                return nullptr;
            }
            return &record;
        }

        /// Inserts the supplied value or range at the requested position.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Asset insert(Record record) {
            record.loaded = true;
            records.push_back(std::move(record));
            return asset_for(records.size() - 1, records.back());
        }

        /// Destroys or releases the `AssetManager` resource represented by the supplied parameters.
        ///
        /// @param record `record` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(Record &record) noexcept {
            if (!record.loaded) {
                return;
            }
            if (auto *model = std::get_if<ModelData>(&record.data)) {
                for (ModelPrimitiveData &primitive : model->primitives) {
                    if (primitive.material) {
                        renderer.destroy_material_instance(primitive.material);
                    }
                    if (primitive.mesh) {
                        renderer.destroy_mesh(primitive.mesh);
                    }
                }
            } else if (auto *texture = std::get_if<TextureData>(&record.data)) {
                if (texture->texture) {
                    renderer.destroy_texture(texture->texture);
                }
            } else if (auto *shader = std::get_if<ShaderData>(&record.data)) {
                if (shader->material_template) {
                    renderer.destroy_material_template(shader->material_template);
                }
            }


            if (!record.source.empty()) {
                const std::string source_string = record.source.string();
                path_cache.erase(source_string);
                path_cache.erase(source_string + "|srgb");
                path_cache.erase(source_string + "|linear");
            }

            record.data = std::monostate{};
            record.loaded = false;
            record.memory_bytes = 0;
            record.label.clear();
            record.source.clear();
            ++record.generation;
            if (record.generation == 0) {
                record.generation = 1;
            }
        }

        SFT::Renderer::Renderer &renderer;
        u64 owner = 0;
        mutable std::shared_mutex mutex;
        std::vector<Record> records;
        TextureStreamer texture_streamer;


        std::unordered_map<std::string, Asset> path_cache;
    };

    /// Performs the asset manager operation for `Engine` using the supplied arguments.
    ///
    /// @param renderer Renderer used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    AssetManager::AssetManager(SFT::Renderer::Renderer &renderer)
        : impl_(std::make_unique<Impl>(renderer)) {}

    /// Destroys the `Engine` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    AssetManager::~AssetManager() {
        clear();
    }

    /// Loads shader.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::NotFound`.
    AssetExpected<Asset> AssetManager::load_shader(ShaderAssetDesc desc) {
        const Foundation::Stopwatch stopwatch;
        const std::string source_for_log = desc.source.string();
        if (desc.source.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A shader asset requires a source path."));
        }


        if (!std::filesystem::exists(desc.source) && Core::Slang::find_embedded_shader(desc.source.string()) == nullptr) {
            return std::unexpected(error(AssetErrorCode::NotFound,
                                         "Shader source does not exist on disk and has no embedded fallback: '" + desc.source.string() + "'.",
                                         desc.source));
        }
        if (desc.label.empty()) {
            desc.label = path_label(desc.source, "shader");
        }
        if (desc.module_name.empty()) {
            desc.module_name = path_label(desc.source.stem(), "shader_module");
        }
        if (desc.vertex_entry_point.empty()) {
            desc.vertex_entry_point = UString{"vertexMain"_ustr};
        }
        if (desc.fragment_entry_point.empty()) {
            desc.fragment_entry_point = UString{"fragmentMain"_ustr};
        }

        Core::Slang::ShaderCompileOptions options{};
        options.entry_points = {
            Core::Slang::ShaderEntryPointRequest{
                .name = desc.vertex_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Vertex,
            },
            Core::Slang::ShaderEntryPointRequest{
                .name = desc.fragment_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Fragment,
            },
        };
        if (!desc.depth_only_fragment_entry_point.empty()) {


            options.entry_points.push_back(Core::Slang::ShaderEntryPointRequest{
                .name = desc.depth_only_fragment_entry_point.cpp_string(),
                .stage = Core::Slang::ShaderStage::Fragment,
            });
        }
        options.macros.reserve(desc.defines.size());
        for (const ShaderDefine &define : desc.defines) {
            options.macros.push_back(Core::Slang::ShaderMacro{
                .name = define.name.cpp_string(),
                .value = define.value.cpp_string(),
            });
        }

        const std::string source_path = desc.source.string();
        auto material_template = impl_->renderer.create_material_template_from_source(
            Core::Slang::ShaderSource::from_file(source_path, desc.module_name.cpp_string()),
            options,
            desc.label.c_str());
        if (!material_template) {
            return std::unexpected(backend_error(material_template.error(), desc.source));
        }

        usize source_bytes = 0;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(desc.source, size_error);
        if (!size_error && size <= std::numeric_limits<usize>::max()) {
            source_bytes = static_cast<usize>(size);
        }

        Foundation::log_info("AssetManager: loaded shader '{}' ({} bytes) in {}", source_for_log, source_bytes, stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Shader,
            .label = std::move(desc.label),
            .source = std::move(desc.source),
            .memory_bytes = source_bytes,
            .data = Impl::ShaderData{.material_template = *material_template},
        });
    }

    /// Loads shader.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_shader(const std::filesystem::path &source, UString label) {
        return load_shader(ShaderAssetDesc{.source = source, .label = std::move(label)});
    }

    /// Creates a texture from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`.
    AssetExpected<Asset> AssetManager::create_texture(TextureAssetDesc desc) {
        if (desc.width == 0 || desc.height == 0) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A texture asset requires non-zero dimensions."));
        }
        const u64 required_bytes = static_cast<u64>(desc.width) * desc.height * 4u;
        if (required_bytes > std::numeric_limits<usize>::max() || desc.rgba8.size() != required_bytes) {
            return std::unexpected(error(
                AssetErrorCode::InvalidDescription,
                "Texture RGBA8 byte count does not match width * height * 4."));
        }
        if (desc.label.empty()) {
            desc.label = UString{"texture"_ustr};
        }

        const bool srgb = desc.color_space == TextureColorSpace::Srgb;
        RHI::Format format = srgb ? RHI::Format::RGBA8UnormSrgb : RHI::Format::RGBA8Unorm;

        Detail::TextureMipChain mip_chain{};
        if (desc.generate_mipmaps) {
            auto generated = Detail::generate_rgba8_mip_chain(desc.rgba8, desc.width, desc.height, srgb);
            if (!generated) {
                return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                             "Could not generate the texture mip chain."));
            }
            mip_chain = std::move(*generated);
        } else {
            mip_chain.data = std::move(desc.rgba8);
            mip_chain.mip_levels = 1;
        }
        std::vector<std::byte>{}.swap(desc.rgba8);
        std::span<const std::byte> upload_bytes{mip_chain.data.data(), mip_chain.data.size()};


        std::optional<std::vector<std::byte>> compressed;
        RHI::RhiDevice *device = impl_->renderer.rhi_device();
        if (desc.allow_compression && desc.width >= 4 && desc.height >= 4 && device != nullptr &&
            device->limits().supports_bc_texture_compression) {
            switch (desc.kind) {
                case TextureKind::ColorOpaque:
                    compressed = Detail::compress_bc1_mip_chain(
                        upload_bytes, desc.width, desc.height, mip_chain.mip_levels, srgb);
                    break;
                case TextureKind::Mask:
                    compressed = Detail::compress_bc4_mip_chain(
                        upload_bytes, desc.width, desc.height, mip_chain.mip_levels);
                    break;
                case TextureKind::NormalMap:
                case TextureKind::MetallicRoughness:


                    compressed = Detail::compress_bc5_mip_chain(
                        upload_bytes, desc.width, desc.height, mip_chain.mip_levels);
                    break;
                case TextureKind::ColorAlpha:
                default:
                    compressed = Detail::compress_bc7_mip_chain(
                        upload_bytes, desc.width, desc.height, mip_chain.mip_levels, srgb);
                    break;
            }
            if (compressed) {
                format = Detail::choose_bc_format(desc.kind, srgb);
                upload_bytes = std::span<const std::byte>{compressed->data(), compressed->size()};
                std::vector<std::byte>{}.swap(mip_chain.data);
            }
        }

        auto texture = impl_->renderer.create_texture(
            desc.width,
            desc.height,
            format,
            upload_bytes,
            desc.label.c_str(),
            {},
            mip_chain.mip_levels);
        if (!texture) {
            return std::unexpected(backend_error(texture.error()));
        }

        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Texture,
            .label = std::move(desc.label),
            .memory_bytes = upload_bytes.size(),
            .data = Impl::TextureData{
                .texture = *texture,
                .info = TextureAssetInfo{
                    .width = desc.width,
                    .height = desc.height,
                    .color_space = desc.color_space,
                },
            },
        });
    }

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
    AssetExpected<Asset> AssetManager::create_orm_texture(
        std::span<const std::byte> occlusion_rgba8, std::span<const std::byte> metallic_roughness_rgba8,
        u32 width, u32 height, UString label) {
        auto packed = Detail::pack_orm_rgba8(occlusion_rgba8, metallic_roughness_rgba8, width, height);
        if (!packed) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Could not pack occlusion + metallic-roughness textures into one "
                                         "ORM texture (dimensions must match exactly)."));
        }
        if (label.empty()) {
            label = UString{"orm_texture"_ustr};
        }


        return create_texture(TextureAssetDesc{
            .width = width,
            .height = height,
            .color_space = TextureColorSpace::Linear,
            .rgba8 = std::move(*packed),
            .label = std::move(label),
            .kind = TextureKind::ColorAlpha,
        });
    }

    /// Loads texture.
    ///
    /// @param source Source value or resource.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_texture(const std::filesystem::path &source,
                                                    TextureColorSpace color_space,
                                                    TextureKind kind,
                                                    UString label) {
        const std::string dedup_key = source.string() +
            (color_space == TextureColorSpace::Srgb ? "|srgb" : "|linear") +
            "|kind" + std::to_string(static_cast<int>(kind));
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        auto encoded = read_binary_file(source);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto decoded = Detail::decode_image_rgba8(*encoded, source);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }

        if (label.empty()) {
            label = path_label(source, "texture");
        }
        const u32 decoded_width = decoded->width;
        const u32 decoded_height = decoded->height;
        auto texture = create_texture(TextureAssetDesc{
            .width = decoded->width,
            .height = decoded->height,
            .color_space = color_space,
            .rgba8 = std::move(decoded->pixels),
            .label = std::move(label),
            .kind = kind,
        });
        if (!texture) {
            return texture;
        }
        Foundation::log_info("AssetManager: loaded texture '{}' ({}x{}) in {}",
                             source.string(),
                             decoded_width,
                             decoded_height,
                             stopwatch.elapsed_human());

        std::unique_lock lock{impl_->mutex};
        if (Impl::Record *record = impl_->find(*texture)) {
            record->source = source;
        }
        impl_->path_cache.emplace(dedup_key, *texture);
        return texture;
    }

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
    AssetExpected<Asset> AssetManager::load_texture_streamed(const std::filesystem::path &source,
                                                              TextureColorSpace color_space, TextureKind kind,
                                                              UString label) {
        if (label.empty()) {
            label = path_label(source, "texture");
        }
        const StreamedTextureHandle streamed =
            impl_->texture_streamer.request_texture_load(source, color_space, kind, {}, label);
        if (!streamed) {
            return std::unexpected(error(AssetErrorCode::IoFailure,
                                         "Failed to start streamed texture load for '" + source.string() + "'.", source));
        }
        const auto [width, height] = impl_->texture_streamer.dimensions(streamed);

        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Texture,
            .label = std::move(label),
            .source = source,
            .data = Impl::TextureData{
                .texture = impl_->texture_streamer.texture_handle(streamed),
                .info = TextureAssetInfo{.width = width, .height = height, .color_space = color_space},
            },
        });
    }

    /// Pumps texture streaming using the supplied arguments and current state.
    ///
    /// @return Returns the current pump texture streaming value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void AssetManager::pump_texture_streaming() {
        impl_->texture_streamer.pump();
    }

    /// Computes the create texture from encoded bytes required by the supplied values.
    ///
    /// @param encoded `encoded` value used by the operation.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::create_texture_from_encoded_bytes(
        std::span<const std::byte> encoded,
        TextureColorSpace color_space,
        TextureKind kind,
        UString label) {
        auto decoded = Detail::decode_image_rgba8(encoded, {});
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (label.empty()) {
            label = UString{"texture"_ustr};
        }
        return create_texture(TextureAssetDesc{
            .width = decoded->width,
            .height = decoded->height,
            .color_space = color_space,
            .rgba8 = std::move(decoded->pixels),
            .label = std::move(label),
            .kind = kind,
        });
    }

    /// Loads sound.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`.
    AssetExpected<Asset> AssetManager::load_sound(const std::filesystem::path &source, UString label) {
        const std::string dedup_key = source.string();
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        auto encoded = read_binary_file(source);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }

        ma_decoder decoder{};
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        const ma_result initialized = ma_decoder_init_memory(
            encoded->data(), encoded->size(), &config, &decoder);
        if (initialized != MA_SUCCESS) {
            return std::unexpected(error(
                AssetErrorCode::DecodeFailure,
                "Could not decode sound '" + source.string() + "': " + ma_result_description(initialized),
                source));
        }

        ma_uint64 frame_count = 0;
        const ma_result length_result = ma_decoder_get_length_in_pcm_frames(&decoder, &frame_count);
        if (length_result != MA_SUCCESS || decoder.outputChannels == 0 || decoder.outputSampleRate == 0 ||
            frame_count > std::numeric_limits<usize>::max() / decoder.outputChannels) {
            ma_decoder_uninit(&decoder);
            return std::unexpected(error(
                AssetErrorCode::DecodeFailure,
                "Could not determine decoded sound dimensions for '" + source.string() + "'.",
                source));
        }

        auto samples = std::make_shared<std::vector<f32>>(
            static_cast<usize>(frame_count * decoder.outputChannels));
        ma_uint64 frames_read = 0;
        const ma_result read_result = ma_decoder_read_pcm_frames(
            &decoder, samples->data(), frame_count, &frames_read);
        const u32 channels = decoder.outputChannels;
        const u32 sample_rate = decoder.outputSampleRate;
        ma_decoder_uninit(&decoder);
        if (read_result != MA_SUCCESS && read_result != MA_AT_END) {
            return std::unexpected(error(
                AssetErrorCode::DecodeFailure,
                "Could not read decoded samples for '" + source.string() + "': " + ma_result_description(read_result),
                source));
        }
        samples->resize(static_cast<usize>(frames_read * channels));

        if (label.empty()) {
            label = path_label(source, "sound");
        }
        Foundation::log_info("AssetManager: loaded sound '{}' ({} frames, {}ch @ {}Hz) in {}",
                             source.string(),
                             frames_read,
                             channels,
                             sample_rate,
                             stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::Sound,
            .label = std::move(label),
            .source = source,
            .memory_bytes = samples->size() * sizeof(f32),
            .data = Impl::SoundData{
                .samples = std::move(samples),
                .info = SoundAssetInfo{
                    .channels = channels,
                    .sample_rate = sample_rate,
                    .frame_count = frames_read,
                    .duration_seconds = sample_rate != 0
                                            ? static_cast<f64>(frames_read) / sample_rate
                                            : 0.0,
                },
            },
        });
        impl_->path_cache.emplace(dedup_key, asset);
        return asset;
    }

    /// Loads file.
    ///
    /// @param source Source value or resource.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::load_file(const std::filesystem::path &source, UString label) {
        const std::string dedup_key = source.string();
        {
            std::shared_lock read_lock{impl_->mutex};
            if (const auto it = impl_->path_cache.find(dedup_key); it != impl_->path_cache.end()) {
                return it->second;
            }
        }

        const Foundation::Stopwatch stopwatch;
        auto bytes = read_binary_file(source);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (label.empty()) {
            label = path_label(source, "file");
        }
        auto shared = std::make_shared<std::vector<std::byte>>(std::move(*bytes));
        Foundation::log_info("AssetManager: loaded file '{}' ({} bytes) in {}", source.string(), shared->size(), stopwatch.elapsed_human());
        std::unique_lock lock{impl_->mutex};
        const Asset asset = impl_->insert(Impl::Record{
            .type = AssetType::File,
            .label = std::move(label),
            .source = source,
            .memory_bytes = shared->size(),
            .data = Impl::FileData{.bytes = std::move(shared)},
        });
        impl_->path_cache.emplace(dedup_key, asset);
        return asset;
    }

    /// Creates a model from the supplied parameters.
    ///
    /// @param mesh `mesh` value used by the operation.
    /// @param shader Shader used or affected by the operation.
    /// @param vertex_color `vertex_color` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    AssetExpected<Asset> AssetManager::create_model(SFT::Renderer::Mesh mesh,
                                                    Asset shader,
                                                    std::optional<glm::vec4> vertex_color,
                                                    UString label) {
        if (label.empty() && !mesh.label().empty()) {
            if (auto mesh_label = UString::try_from_utf8(mesh.label())) {
                label = std::move(*mesh_label);
            }
        }
        return create_model(ModelAssetDesc{
            .label = std::move(label),
            .primitives = {
                ModelPrimitiveDesc{
                    .mesh = std::move(mesh),
                    .shader = shader,
                    .vertex_color = vertex_color,
                },
            },
        });
    }

    /// Creates a model from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidDescription`, `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<Asset> AssetManager::create_model(ModelAssetDesc desc) {
        const Foundation::Stopwatch stopwatch;
        if (desc.primitives.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A model asset requires at least one primitive."));
        }
        if (desc.label.empty()) {
            desc.label = UString{"model"_ustr};
        }

        std::unique_lock lock{impl_->mutex};
        Impl::ModelData model{};
        model.primitives.reserve(desc.primitives.size());

        const auto rollback = [this, &model]() noexcept {
            for (Impl::ModelPrimitiveData &created : model.primitives) {
                impl_->renderer.destroy_material_instance(created.material);
                impl_->renderer.destroy_mesh(created.mesh);
            }
            model.primitives.clear();
        };

        for (usize primitive_index = 0; primitive_index < desc.primitives.size(); ++primitive_index) {
            ModelPrimitiveDesc &primitive = desc.primitives[primitive_index];
            const Impl::Record *shader_record = impl_->find(primitive.shader);
            const auto *shader = shader_record ? std::get_if<Impl::ShaderData>(&shader_record->data) : nullptr;
            if (shader == nullptr) {
                rollback();
                return std::unexpected(error(
                    primitive.shader && primitive.shader.type() != AssetType::Shader
                        ? AssetErrorCode::WrongType
                        : AssetErrorCode::InvalidAsset,
                    "Model primitive references an invalid shader asset."));
            }

            if (primitive.vertex_color) {
                primitive.mesh.set_vertex_color(*primitive.vertex_color);
            }
            model.info.vertex_count += primitive.mesh.vertices().size();
            model.info.index_count += primitive.mesh.indices().size();

            auto mesh = impl_->renderer.upload(primitive.mesh);
            if (!mesh) {
                rollback();
                return std::unexpected(backend_error(mesh.error()));
            }

            const std::string material_label = desc.label.cpp_string() + " material " +
                                               std::to_string(primitive_index);
            auto material = impl_->renderer.create_material_instance(
                shader->material_template,
                material_label.c_str());
            if (!material) {
                impl_->renderer.destroy_mesh(*mesh);
                rollback();
                return std::unexpected(backend_error(material.error()));
            }

            Impl::ModelPrimitiveData created{
                .mesh = *mesh,
                .material = *material,
                .shader = primitive.shader,
                .double_sided = primitive.double_sided,
            };
            created.textures.reserve(primitive.textures.size());
            for (const ModelTextureBinding &binding : primitive.textures) {
                const Impl::Record *texture_record = impl_->find(binding.texture);
                const auto *texture = texture_record ? std::get_if<Impl::TextureData>(&texture_record->data) : nullptr;
                if (texture == nullptr) {
                    impl_->renderer.destroy_material_instance(created.material);
                    impl_->renderer.destroy_mesh(created.mesh);
                    rollback();
                    return std::unexpected(error(
                        binding.texture && binding.texture.type() != AssetType::Texture
                            ? AssetErrorCode::WrongType
                            : AssetErrorCode::InvalidAsset,
                        "Model primitive references an invalid texture asset."));
                }
                if (Core::RendererResult bound = impl_->renderer.set_material_texture(
                        created.material, binding.slot.cpp_string_view(), texture->texture);
                    !bound) {
                    impl_->renderer.destroy_material_instance(created.material);
                    impl_->renderer.destroy_mesh(created.mesh);
                    rollback();
                    return std::unexpected(backend_error(bound.error()));
                }
                created.textures.push_back(binding.texture);
            }
            model.primitives.push_back(std::move(created));
        }
        model.info.primitive_count = model.primitives.size();
        model.info.triangle_count = model.info.index_count / 3;

        const usize approximate_bytes =
            model.info.vertex_count * sizeof(SFT::Renderer::GeometryVertex) +
            model.info.index_count * sizeof(u32);
        Foundation::log_info("AssetManager: created model '{}' ({} primitive(s), {} tri) in {}",
                             desc.label.cpp_string(),
                             model.info.primitive_count,
                             model.info.triangle_count,
                             stopwatch.elapsed_human());
        return impl_->insert(Impl::Record{
            .type = AssetType::Model,
            .label = std::move(desc.label),
            .memory_bytes = approximate_bytes,
            .data = std::move(model),
        });
    }

    /// Sets the model float for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param name Name used to identify or label the target.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_float(Asset model, usize primitive, std::string_view name, f32 value) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(model);
        auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model
                                             ? AssetErrorCode::WrongType
                                             : AssetErrorCode::InvalidAsset,
                                         "set_model_float requires a live model asset."));
        }
        if (primitive >= data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Core::RendererResult result = impl_->renderer.set_material_float(
            data->primitives[primitive].material, name, value);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        return {};
    }

    /// Sets the model vec4 for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param name Name used to identify or label the target.
    /// @param value Value consumed by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_vec4(Asset model, usize primitive, std::string_view name,
                                             const glm::vec4 &value) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(model);
        auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(model && model.type() != AssetType::Model
                                             ? AssetErrorCode::WrongType
                                             : AssetErrorCode::InvalidAsset,
                                         "set_model_vec4 requires a live model asset."));
        }
        if (primitive >= data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Core::RendererResult result = impl_->renderer.set_material_vec4(
            data->primitives[primitive].material, name, value.x, value.y, value.z, value.w);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        return {};
    }

    /// Sets the model texture for this `Engine`.
    ///
    /// @param model `model` value used by the operation.
    /// @param primitive `primitive` value used by the operation.
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param texture_asset Texture used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InvalidDescription`.
    AssetResult AssetManager::set_model_texture(Asset model, usize primitive, std::string_view slot,
                                                Asset texture_asset) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *model_record = impl_->find(model);
        auto *model_data = model_record ? std::get_if<Impl::ModelData>(&model_record->data) : nullptr;
        Impl::Record *texture_record = impl_->find(texture_asset);
        auto *texture = texture_record ? std::get_if<Impl::TextureData>(&texture_record->data) : nullptr;
        if (model_data == nullptr || texture == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "set_model_texture requires live model and texture assets from this manager."));
        }
        if (primitive >= model_data->primitives.size()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "Model primitive index is out of range."));
        }
        Impl::ModelPrimitiveData &target = model_data->primitives[primitive];
        Core::RendererResult result = impl_->renderer.set_material_texture(target.material, slot, texture->texture);
        if (!result) {
            return std::unexpected(backend_error(result.error()));
        }
        if (std::ranges::find(target.textures, texture_asset) == target.textures.end()) {
            target.textures.push_back(texture_asset);
        }
        return {};
    }

    /// Reports whether contains holds for this `Engine`.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool AssetManager::contains(Asset asset) const noexcept {
        std::shared_lock lock{impl_->mutex};
        return impl_->find(asset) != nullptr;
    }

    /// Returns the size for this `Engine`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    usize AssetManager::size() const noexcept {
        std::shared_lock lock{impl_->mutex};
        return static_cast<usize>(std::ranges::count_if(impl_->records, [](const Impl::Record &record) {
            return record.loaded;
        }));
    }

    /// Performs the info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`.
    AssetExpected<AssetInfo> AssetManager::info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        if (record == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "Asset is stale, unloaded, or belongs to another AssetManager."));
        }
        return AssetInfo{
            .asset = asset,
            .label = record->label,
            .source = record->source,
            .memory_bytes = record->memory_bytes,
            .loaded = record->loaded,
        };
    }

    /// Performs the model info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<ModelAssetInfo> AssetManager::model_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "model_info requires a live model asset."));
        }
        return data->info;
    }

    /// Performs the texture info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<TextureAssetInfo> AssetManager::texture_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "texture_info requires a live texture asset."));
        }
        return data->info;
    }

    /// Returns the texture handle associated with this `Engine`.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<SFT::Renderer::TextureHandle> AssetManager::texture_handle(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::TextureData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "texture_handle requires a live texture asset."));
        }
        return data->texture;
    }

    /// Performs the sound info operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<SoundAssetInfo> AssetManager::sound_info(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "sound_info requires a live sound asset."));
        }
        return data->info;
    }

    /// Computes the file bytes required by the supplied values.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<std::shared_ptr<const std::vector<std::byte>>> AssetManager::file_bytes(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::FileData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "file_bytes requires a live file asset."));
        }
        return std::shared_ptr<const std::vector<std::byte>>{data->bytes};
    }

    /// Performs the sound samples operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::WrongType`, `AssetErrorCode::InvalidAsset`.
    AssetExpected<std::shared_ptr<const std::vector<f32>>> AssetManager::sound_samples(Asset asset) const {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(asset);
        const auto *data = record ? std::get_if<Impl::SoundData>(&record->data) : nullptr;
        if (data == nullptr) {
            return std::unexpected(error(record ? AssetErrorCode::WrongType : AssetErrorCode::InvalidAsset,
                                         "sound_samples requires a live sound asset."));
        }
        return std::shared_ptr<const std::vector<f32>>{data->samples};
    }

    /// Performs the unload operation for `Engine` using the supplied arguments.
    ///
    /// @param asset `asset` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::InvalidAsset`, `AssetErrorCode::InUse`.
    AssetResult AssetManager::unload(Asset asset) {
        std::unique_lock lock{impl_->mutex};
        Impl::Record *record = impl_->find(asset);
        if (record == nullptr) {
            return std::unexpected(error(AssetErrorCode::InvalidAsset,
                                         "Asset is stale, unloaded, or belongs to another AssetManager."));
        }

        if (record->type == AssetType::Shader || record->type == AssetType::Texture) {
            for (const Impl::Record &candidate : impl_->records) {
                const auto *model = candidate.loaded ? std::get_if<Impl::ModelData>(&candidate.data) : nullptr;
                if (model == nullptr) {
                    continue;
                }
                for (const Impl::ModelPrimitiveData &primitive : model->primitives) {
                    const bool referenced = record->type == AssetType::Shader
                                                ? primitive.shader == asset
                                                : std::ranges::find(primitive.textures, asset) != primitive.textures.end();
                    if (referenced) {
                        return std::unexpected(error(
                            AssetErrorCode::InUse,
                            "Cannot unload a " + std::string{to_string(record->type)} +
                                " asset while a loaded model depends on it."));
                    }
                }
            }
        }

        impl_->destroy(*record);
        return {};
    }

    /// Clears the stored state or contents.
    ///
    /// @return Returns the current clear value.
    /// @note This function does not throw exceptions.
    void AssetManager::clear() noexcept {
        if (!impl_) {
            return;
        }
        std::unique_lock lock{impl_->mutex};

        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Model) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Texture) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            if (record.type == AssetType::Shader) {
                impl_->destroy(record);
            }
        }
        for (Impl::Record &record : impl_->records) {
            impl_->destroy(record);
        }
        impl_->records.clear();
    }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param model_asset `model_asset` value used by the operation.
    /// @param world_transform World used or affected by the operation.
    /// @param stable_id Identifier of the target object or resource.
    /// @param visibility_mask `visibility_mask` value used by the operation.
    /// @param sort_key Key used to identify the requested entry.
    /// @param destination Destination value or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool AssetManager::append_model_renderables(
        Asset model_asset,
        const glm::mat4 &world_transform,
        u64 stable_id,
        u32 visibility_mask,
        u32 sort_key,
        std::vector<SFT::Renderer::SceneRenderable> &destination) const noexcept {
        std::shared_lock lock{impl_->mutex};
        const Impl::Record *record = impl_->find(model_asset);
        const auto *model = record ? std::get_if<Impl::ModelData>(&record->data) : nullptr;
        if (model == nullptr) {
            return false;
        }
        destination.reserve(destination.size() + model->primitives.size());
        for (usize primitive_index = 0; primitive_index < model->primitives.size(); ++primitive_index) {
            const Impl::ModelPrimitiveData &primitive = model->primitives[primitive_index];
            destination.push_back(SFT::Renderer::SceneRenderable{
                .mesh = primitive.mesh,
                .material = primitive.material,
                .world_transform = world_transform,
                .stable_id = combine_stable_id(stable_id, primitive_index),
                .visibility_mask = visibility_mask,
                .sort_key = sort_key + static_cast<u32>(primitive_index),
                .cull_mode = primitive.double_sided ? RHI::CullMode::None : RHI::CullMode::Back,
            });
        }
        return true;
    }

} // namespace SFT::Engine
