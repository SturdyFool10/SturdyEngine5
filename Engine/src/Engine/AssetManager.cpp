#include "AssetManager.hpp"
#include "ImageDecode.hpp"

#include <Core/Core.hpp>
#include <Renderer/Renderer.hpp>

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <variant>

namespace SFT::Engine {

    namespace {

        std::atomic<u64> next_asset_manager_id{1};

        [[nodiscard]] UString path_label(const std::filesystem::path &path, std::string_view fallback) {
            const std::string name = path.filename().string();
            if (auto utf8 = UString::try_from_utf8(name)) {
                return std::move(*utf8);
            }
            return UString{fallback};
        }

        [[nodiscard]] AssetError error(AssetErrorCode code,
                                       std::string message,
                                       std::filesystem::path source = {}) {
            return AssetError{
                .code = code,
                .message = UString{message},
                .source = std::move(source),
            };
        }

        [[nodiscard]] AssetError backend_error(const Core::GraphicsBackendError &failure,
                                               const std::filesystem::path &source = {}) {
            return error(AssetErrorCode::BackendFailure, failure.message, source);
        }

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

        [[nodiscard]] u64 combine_stable_id(u64 object, usize primitive) noexcept {
            // 64-bit hash-combine keeps each primitive stable without imposing an artificial eight-bit
            // primitive limit or exposing renderer IDs to the ECS.
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

        explicit Impl(SFT::Renderer::Renderer &renderer_ref)
            : renderer(renderer_ref), owner(next_asset_manager_id.fetch_add(1, std::memory_order_relaxed)) {
            if (owner == 0) {
                owner = next_asset_manager_id.fetch_add(1, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] Asset asset_for(usize index, const Record &record) const noexcept {
            return Asset{owner, static_cast<u64>(index + 1), record.generation, record.type};
        }

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

        [[nodiscard]] Asset insert(Record record) {
            record.loaded = true;
            records.push_back(std::move(record));
            return asset_for(records.size() - 1, records.back());
        }

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
    };

    AssetManager::AssetManager(SFT::Renderer::Renderer &renderer)
        : impl_(std::make_unique<Impl>(renderer)) {}

    AssetManager::~AssetManager() {
        clear();
    }

    AssetExpected<Asset> AssetManager::load_shader(ShaderAssetDesc desc) {
        if (desc.source.empty()) {
            return std::unexpected(error(AssetErrorCode::InvalidDescription,
                                         "A shader asset requires a source path."));
        }
        if (!std::filesystem::exists(desc.source)) {
            return std::unexpected(error(AssetErrorCode::NotFound,
                                         "Shader source does not exist: '" + desc.source.string() + "'.",
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
            // A second Fragment-stage entry point from the same module — build_material_template_gpu
            // (RendererMaterial.cpp) picks the first reflected Fragment entry as the main fragment and
            // this second one as the depth-only entry, by request order.
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

        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Shader,
            .label = std::move(desc.label),
            .source = std::move(desc.source),
            .memory_bytes = source_bytes,
            .data = Impl::ShaderData{.material_template = *material_template},
        });
    }

    AssetExpected<Asset> AssetManager::load_shader(const std::filesystem::path &source, UString label) {
        return load_shader(ShaderAssetDesc{.source = source, .label = std::move(label)});
    }

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

        const RHI::Format format = desc.color_space == TextureColorSpace::Srgb
                                       ? RHI::Format::RGBA8UnormSrgb
                                       : RHI::Format::RGBA8Unorm;
        auto texture = impl_->renderer.create_texture(
            desc.width,
            desc.height,
            format,
            std::span<const std::byte>{desc.rgba8.data(), desc.rgba8.size()},
            desc.label.c_str());
        if (!texture) {
            return std::unexpected(backend_error(texture.error()));
        }

        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::Texture,
            .label = std::move(desc.label),
            .memory_bytes = static_cast<usize>(required_bytes),
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

    AssetExpected<Asset> AssetManager::load_texture(const std::filesystem::path &source,
                                                    TextureColorSpace color_space,
                                                    UString label) {
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
        auto texture = create_texture(TextureAssetDesc{
            .width = decoded->width,
            .height = decoded->height,
            .color_space = color_space,
            .rgba8 = std::move(decoded->pixels),
            .label = std::move(label),
        });
        if (!texture) {
            return texture;
        }

        std::unique_lock lock{impl_->mutex};
        if (Impl::Record *record = impl_->find(*texture)) {
            record->source = source;
        }
        return texture;
    }

    AssetExpected<Asset> AssetManager::create_texture_from_encoded_bytes(
        std::span<const std::byte> encoded,
        TextureColorSpace color_space,
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
        });
    }

    AssetExpected<Asset> AssetManager::load_sound(const std::filesystem::path &source, UString label) {
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
        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
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
    }

    AssetExpected<Asset> AssetManager::load_file(const std::filesystem::path &source, UString label) {
        auto bytes = read_binary_file(source);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (label.empty()) {
            label = path_label(source, "file");
        }
        auto shared = std::make_shared<std::vector<std::byte>>(std::move(*bytes));
        std::unique_lock lock{impl_->mutex};
        return impl_->insert(Impl::Record{
            .type = AssetType::File,
            .label = std::move(label),
            .source = source,
            .memory_bytes = shared->size(),
            .data = Impl::FileData{.bytes = std::move(shared)},
        });
    }

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

    AssetExpected<Asset> AssetManager::create_model(ModelAssetDesc desc) {
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

        const usize approximate_bytes =
            model.info.vertex_count * sizeof(SFT::Renderer::GeometryVertex) +
            model.info.index_count * sizeof(u32);
        return impl_->insert(Impl::Record{
            .type = AssetType::Model,
            .label = std::move(desc.label),
            .memory_bytes = approximate_bytes,
            .data = std::move(model),
        });
    }

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

    bool AssetManager::contains(Asset asset) const noexcept {
        std::shared_lock lock{impl_->mutex};
        return impl_->find(asset) != nullptr;
    }

    usize AssetManager::size() const noexcept {
        std::shared_lock lock{impl_->mutex};
        return static_cast<usize>(std::ranges::count_if(impl_->records, [](const Impl::Record &record) {
            return record.loaded;
        }));
    }

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

    void AssetManager::clear() noexcept {
        if (!impl_) {
            return;
        }
        std::unique_lock lock{impl_->mutex};
        // Dependency order: model material instances first, then their textures and shader templates.
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
            });
        }
        return true;
    }

} // namespace SFT::Engine
