#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <Engine/GltfImport.hpp>

#include <Engine/AssetManager.hpp>
#include <Engine/ImageDecode.hpp>
#include <Engine/ImportCommon.hpp>
#include <Engine/TextureCompression.hpp>

#include <Renderer/Mesh.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] AssetError gltf_error(AssetErrorCode code, std::string message,
                                            std::filesystem::path source = {}) {
            return AssetError{.code = code, .message = UString{message}, .source = std::move(source)};
        }

        /// Performs the cgltf result message operation for `Engine` using the supplied arguments.
        ///
        /// @param result `result` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string_view cgltf_result_message(cgltf_result result) noexcept {
            switch (result) {
                case cgltf_result_data_too_short: return "data too short";
                case cgltf_result_unknown_format: return "unknown format";
                case cgltf_result_invalid_json: return "invalid JSON";
                case cgltf_result_invalid_gltf: return "invalid glTF";
                case cgltf_result_invalid_options: return "invalid options";
                case cgltf_result_file_not_found: return "file not found";
                case cgltf_result_io_error: return "I/O error";
                case cgltf_result_out_of_memory: return "out of memory";
                case cgltf_result_legacy_gltf: return "legacy (pre-2.0) glTF is not supported";
                default: return "unknown error";
            }
        }

        /// Returns a human-readable name for the supplied label from value.
        ///
        /// @param name Name used to identify or label the target.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString label_from_name(const char *name, std::string_view fallback) {
            if (name != nullptr && name[0] != '\0') {
                if (auto converted = UString::try_from_utf8(std::string_view{name})) {
                    return std::move(*converted);
                }
            }
            return UString{fallback};
        }

        struct CgltfDataGuard {
            cgltf_data *data = nullptr;
            /// Destroys the `CgltfDataGuard` and releases resources owned by it.
            ///
            /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
            ~CgltfDataGuard() {
                if (data != nullptr) {
                    cgltf_free(data);
                }
            }
        };


        struct ImageCache {
            std::vector<std::array<std::array<std::optional<Asset>, 5>, 2>> entries;
        };

        /// Performs the color space slot operation for `Engine` using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize color_space_slot(TextureColorSpace color_space) noexcept {
            return color_space == TextureColorSpace::Srgb ? 0 : 1;
        }

        /// Performs the kind slot operation for `Engine` using the supplied arguments.
        ///
        /// @param kind `kind` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize kind_slot(TextureKind kind) noexcept {
            return static_cast<usize>(kind);
        }


        /// Computes the fetch gltf image bytes required by the supplied values.
        ///
        /// @param image `image` value used by the operation.
        /// @param base_dir `base_dir` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::DecodeFailure`, `AssetErrorCode::InvalidDescription`, `AssetErrorCode::IoFailure`.
        [[nodiscard]] AssetExpected<std::vector<std::byte>> fetch_gltf_image_bytes(
            const cgltf_image &image, const std::filesystem::path &base_dir) {
            if (image.buffer_view != nullptr) {
                const std::uint8_t *bytes = cgltf_buffer_view_data(image.buffer_view);
                if (bytes == nullptr) {
                    return std::unexpected(gltf_error(AssetErrorCode::DecodeFailure,
                                                       "glTF embedded image has no backing buffer data."));
                }
                return std::vector<std::byte>{
                    reinterpret_cast<const std::byte *>(bytes),
                    reinterpret_cast<const std::byte *>(bytes) + image.buffer_view->size};
            }
            if (image.uri == nullptr) {
                return std::unexpected(gltf_error(AssetErrorCode::InvalidDescription,
                                                   "glTF image has neither a buffer view nor a URI."));
            }
            if (std::string_view{image.uri}.starts_with("data:")) {
                const char *comma = std::strchr(image.uri, ',');
                if (comma == nullptr) {
                    return std::unexpected(gltf_error(AssetErrorCode::DecodeFailure,
                                                       "glTF image data URI is missing its ',' payload separator."));
                }
                const std::string_view payload{comma + 1};
                usize padding = 0;
                if (payload.ends_with("==")) {
                    padding = 2;
                } else if (payload.ends_with('=')) {
                    padding = 1;
                }
                const usize decoded_size = (payload.size() / 4) * 3 - padding;
                cgltf_options base64_options{};
                void *decoded = nullptr;
                const cgltf_result result =
                    cgltf_load_buffer_base64(&base64_options, decoded_size, comma + 1, &decoded);
                if (result != cgltf_result_success || decoded == nullptr) {
                    return std::unexpected(gltf_error(AssetErrorCode::DecodeFailure,
                                                       "Could not base64-decode a glTF image data URI."));
                }
                std::vector<std::byte> bytes{
                    reinterpret_cast<const std::byte *>(decoded),
                    reinterpret_cast<const std::byte *>(decoded) + decoded_size};
                std::free(decoded);
                return bytes;
            }

            std::string decoded_uri = image.uri;
            const cgltf_size decoded_length = cgltf_decode_uri(decoded_uri.data());
            decoded_uri.resize(decoded_length);
            const std::filesystem::path file_path = base_dir / decoded_uri;
            std::ifstream file(file_path, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::unexpected(gltf_error(AssetErrorCode::IoFailure,
                                                   "Could not open glTF image file '" + file_path.string() + "'.",
                                                   file_path));
            }
            const std::streamoff size = file.tellg();
            if (size < 0) {
                return std::unexpected(gltf_error(AssetErrorCode::IoFailure,
                                                   "Could not determine the size of glTF image file '" +
                                                       file_path.string() + "'.",
                                                   file_path));
            }
            file.seekg(0, std::ios::beg);
            std::vector<std::byte> bytes(static_cast<usize>(size));
            if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()),
                                              static_cast<std::streamsize>(bytes.size()))) {
                return std::unexpected(gltf_error(AssetErrorCode::IoFailure,
                                                   "Could not read glTF image file '" + file_path.string() + "'.",
                                                   file_path));
            }
            return bytes;
        }


        /// Decodes gltf image pixels.
        ///
        /// @param image `image` value used by the operation.
        /// @param base_dir `base_dir` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Detail::DecodedImage> decode_gltf_image_pixels(
            const cgltf_image &image, const std::filesystem::path &base_dir) {
            auto encoded = fetch_gltf_image_bytes(image, base_dir);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            return Detail::decode_image_rgba8(*encoded, {});
        }

        /// Loads image.
        ///
        /// @param assets `assets` value used by the operation.
        /// @param image `image` value used by the operation.
        /// @param image_index Zero-based index of the target element or entry.
        /// @param color_space `color_space` value used by the operation.
        /// @param kind `kind` value used by the operation.
        /// @param base_dir `base_dir` value used by the operation.
        /// @param cache `cache` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] AssetExpected<Asset> load_image(
            AssetManager &assets,
            const cgltf_image &image,
            usize image_index,
            TextureColorSpace color_space,
            TextureKind kind,
            const std::filesystem::path &base_dir,
            ImageCache &cache) {
            const usize cs_slot = color_space_slot(color_space);
            const usize k_slot = kind_slot(kind);
            if (std::optional<Asset> &cached = cache.entries[image_index][cs_slot][k_slot]; cached) {
                return *cached;
            }

            const UString label = label_from_name(image.name, "gltf_image");

            AssetExpected<Asset> loaded = [&]() -> AssetExpected<Asset> {


                if (image.buffer_view == nullptr && image.uri != nullptr &&
                    !std::string_view{image.uri}.starts_with("data:")) {
                    std::string decoded_uri = image.uri;
                    const cgltf_size decoded_length = cgltf_decode_uri(decoded_uri.data());
                    decoded_uri.resize(decoded_length);
                    return assets.load_texture(base_dir / decoded_uri, color_space, kind, label);
                }

                auto encoded = fetch_gltf_image_bytes(image, base_dir);
                if (!encoded) {
                    return std::unexpected(encoded.error());
                }
                return assets.create_texture_from_encoded_bytes(*encoded, color_space, kind, label);
            }();

            if (loaded) {
                cache.entries[image_index][cs_slot][k_slot] = *loaded;
            }
            return loaded;
        }


        [[nodiscard]] Animation::JointTransform node_local_transform(const cgltf_node &node) {
            Animation::JointTransform t;
            if (node.has_matrix) {
                return Detail::decompose_transform(glm::make_mat4(node.matrix));
            }
            if (node.has_translation) {
                t.translation = {node.translation[0], node.translation[1], node.translation[2]};
            }
            if (node.has_rotation) {
                t.rotation = glm::normalize(glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]));
            }
            if (node.has_scale) {
                t.scale = {node.scale[0], node.scale[1], node.scale[2]};
            }
            return t;
        }

        struct BuiltSkin {
            GltfSkin skin;
            std::vector<const cgltf_node *> nodes;       // skeleton joint index -> node
            std::vector<u32> skin_joint_to_skeleton;     // skin.joints[i] -> skeleton joint index
        };

        [[nodiscard]] Animation::Interpolation lower_interpolation(cgltf_interpolation_type type) noexcept {
            switch (type) {
                case cgltf_interpolation_type_step: return Animation::Interpolation::Step;
                case cgltf_interpolation_type_cubic_spline: return Animation::Interpolation::CubicSpline;
                default: return Animation::Interpolation::Linear;
            }
        }

        /// One glTF animation addressed to `nodes` (joint i of the result animates nodes[i]). Weight channels
        /// become morph tracks keyed by node name. Null when no channel touches `nodes`.
        [[nodiscard]] std::shared_ptr<Animation::Clip> build_clip(const cgltf_animation &animation,
                                                                  const std::vector<const cgltf_node *> &nodes,
                                                                  const std::vector<const cgltf_node *> &morph_nodes = {}) {
            auto clip = std::make_shared<Animation::Clip>();
            clip->name = animation.name != nullptr ? animation.name : "animation";
            clip->channels.resize(nodes.size());
            for (const cgltf_node *node : nodes) {
                clip->joint_names.emplace_back(node->name != nullptr ? node->name : "joint");
            }
            bool any = false;
            for (cgltf_size c = 0; c < animation.channels_count; ++c) {
                const cgltf_animation_channel &channel = animation.channels[c];
                if (channel.target_node == nullptr || channel.sampler == nullptr) {
                    continue;
                }
                auto it = std::find(nodes.begin(), nodes.end(), channel.target_node);
                const bool in_nodes = it != nodes.end();
                if (!in_nodes && !(channel.target_path == cgltf_animation_path_type_weights &&
                                   std::find(morph_nodes.begin(), morph_nodes.end(), channel.target_node) != morph_nodes.end())) {
                    continue;
                }
                const cgltf_animation_sampler &sampler = *channel.sampler;
                const Animation::Interpolation interpolation = lower_interpolation(sampler.interpolation);
                std::vector<f32> times(sampler.input->count);
                cgltf_accessor_unpack_floats(sampler.input, times.data(), times.size());
                std::vector<f32> values(sampler.output->count * cgltf_num_components(sampler.output->type));
                cgltf_accessor_unpack_floats(sampler.output, values.data(), values.size());
                const usize tuples = interpolation == Animation::Interpolation::CubicSpline ? 3 : 1;

                if (channel.target_path == cgltf_animation_path_type_weights) {
                    if (times.empty() || values.size() % (times.size() * tuples) != 0) {
                        continue;
                    }
                    Animation::MorphTrack morph;
                    morph.target = channel.target_node->name != nullptr ? channel.target_node->name : "node";
                    morph.weight_count = static_cast<u32>(values.size() / (times.size() * tuples));
                    morph.track = Animation::Track{interpolation, std::move(times), std::move(values)};
                    clip->morph_tracks.push_back(std::move(morph));
                    any = true;
                    continue;
                }

                if (!in_nodes) {
                    continue;
                }
                Animation::JointChannels &joint = clip->channels[static_cast<usize>(it - nodes.begin())];
                Animation::Track *track = nullptr;
                usize components = 3;
                switch (channel.target_path) {
                    case cgltf_animation_path_type_translation: track = &joint.translation; break;
                    case cgltf_animation_path_type_rotation: track = &joint.rotation; components = 4; break;
                    case cgltf_animation_path_type_scale: track = &joint.scale; break;
                    default: break;
                }
                if (track == nullptr || values.size() != times.size() * components * tuples) {
                    continue; // unsupported path or malformed sampler
                }
                *track = Animation::Track{interpolation, std::move(times), std::move(values)};
                any = true;
            }
            if (!any) {
                return nullptr;
            }
            clip->recompute_duration();
            return clip;
        }

        [[nodiscard]] BuiltSkin build_skin(const cgltf_data &data, const cgltf_skin &skin) {
            BuiltSkin built;
            // Joints plus every ancestor, so the hierarchy is connected; ordered by depth so parents come first.
            std::vector<const cgltf_node *> included;
            const auto add = [&included](const cgltf_node *node) {
                if (std::find(included.begin(), included.end(), node) == included.end()) {
                    included.push_back(node);
                }
            };
            for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                for (const cgltf_node *n = skin.joints[j]; n != nullptr; n = n->parent) {
                    add(n);
                }
            }
            const auto depth = [](const cgltf_node *n) {
                usize d = 0;
                for (; n->parent != nullptr; n = n->parent) {
                    ++d;
                }
                return d;
            };
            std::stable_sort(included.begin(), included.end(),
                             [&depth](const cgltf_node *a, const cgltf_node *b) { return depth(a) < depth(b); });
            built.nodes = included;

            auto skeleton = std::make_shared<Animation::Skeleton>();
            const auto index_of = [&included](const cgltf_node *n) -> u32 {
                const auto it = std::find(included.begin(), included.end(), n);
                return static_cast<u32>(it - included.begin());
            };
            for (const cgltf_node *node : included) {
                skeleton->names.emplace_back(node->name != nullptr ? node->name : "joint");
                skeleton->parents.push_back(node->parent != nullptr ? index_of(node->parent) : Animation::no_joint);
                skeleton->rest_pose.push_back(node_local_transform(*node));
                skeleton->inverse_bind.emplace_back(1.0f);
            }
            built.skin_joint_to_skeleton.resize(skin.joints_count);
            for (cgltf_size j = 0; j < skin.joints_count; ++j) {
                const u32 index = index_of(skin.joints[j]);
                built.skin_joint_to_skeleton[j] = index;
                if (skin.inverse_bind_matrices != nullptr) {
                    glm::mat4 ibm{1.0f};
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, j, glm::value_ptr(ibm), 16);
                    skeleton->inverse_bind[index] = ibm;
                }
            }

            std::vector<const cgltf_node *> mesh_nodes;
            for (cgltf_size n = 0; n < data.nodes_count; ++n) {
                if (data.nodes[n].skin == &skin && data.nodes[n].mesh != nullptr) {
                    mesh_nodes.push_back(&data.nodes[n]);
                }
            }
            for (cgltf_size a = 0; a < data.animations_count; ++a) {
                if (auto clip = build_clip(data.animations[a], included, mesh_nodes)) {
                    built.skin.clips.push_back(std::move(clip));
                }
            }
            built.skin.name = label_from_name(skin.name, "gltf_skin");
            built.skin.skeleton = std::move(skeleton);
            return built;
        }

        struct SceneRig {
            std::shared_ptr<const Animation::Skeleton> skeleton;
            std::vector<u32> node_to_joint; // glTF node index -> joint
            std::vector<std::shared_ptr<const Animation::Clip>> clips;
        };

        [[nodiscard]] SceneRig build_scene_rig(const cgltf_data &data) {
            SceneRig rig;
            if (data.nodes_count == 0) {
                return rig;
            }
            std::vector<const cgltf_node *> ordered(data.nodes_count);
            for (cgltf_size i = 0; i < data.nodes_count; ++i) {
                ordered[i] = &data.nodes[i];
            }
            const auto depth = [](const cgltf_node *n) {
                usize d = 0;
                for (; n->parent != nullptr; n = n->parent) {
                    ++d;
                }
                return d;
            };
            std::stable_sort(ordered.begin(), ordered.end(),
                             [&depth](const cgltf_node *a, const cgltf_node *b) { return depth(a) < depth(b); });
            auto skeleton = std::make_shared<Animation::Skeleton>();
            rig.node_to_joint.assign(data.nodes_count, Animation::no_joint);
            for (usize j = 0; j < ordered.size(); ++j) {
                rig.node_to_joint[static_cast<usize>(ordered[j] - data.nodes)] = static_cast<u32>(j);
            }
            for (const cgltf_node *node : ordered) {
                skeleton->names.emplace_back(node->name != nullptr ? node->name : "node");
                skeleton->parents.push_back(node->parent != nullptr
                                                ? rig.node_to_joint[static_cast<usize>(node->parent - data.nodes)]
                                                : Animation::no_joint);
                skeleton->rest_pose.push_back(node_local_transform(*node));
                skeleton->inverse_bind.emplace_back(1.0f);
            }
            for (cgltf_size a = 0; a < data.animations_count; ++a) {
                if (auto clip = build_clip(data.animations[a], ordered)) {
                    rig.clips.push_back(std::move(clip));
                }
            }
            rig.skeleton = std::move(skeleton);
            return rig;
        }

        constexpr f32 kPhotometricToRadiometric = 1.0f / 683.0f;

        /// Collects node instances using the supplied arguments and current state.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param node `node` value used by the operation.
        /// @param models `models` value used by the operation.
        /// @param instances Instance used or affected by the operation.
        /// @param lights `lights` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void collect_node_instances(
            const cgltf_data &data,
            const cgltf_node &node,
            const std::vector<Asset> &models,
            const std::vector<u32> &node_to_joint,
            std::vector<GltfNodeInstance> &instances,
            std::vector<GltfLightInstance> &lights) {
            if (node.mesh != nullptr && static_cast<usize>(node.mesh - data.meshes) < models.size()) {
                const auto mesh_index = static_cast<usize>(node.mesh - data.meshes);
                glm::mat4 world{1.0f};
                const bool skinned = node.skin != nullptr;
                if (!skinned) {
                    cgltf_node_transform_world(&node, glm::value_ptr(world));
                }
                instances.push_back(GltfNodeInstance{
                    .name = label_from_name(node.name, "gltf_node"),
                    .model = models[mesh_index],
                    .world_transform = world,
                    .skin = skinned ? static_cast<i32>(node.skin - data.skins) : -1,
                    .scene_joint = node_to_joint.empty() || node_to_joint[static_cast<usize>(&node - data.nodes)] == Animation::no_joint
                                       ? -1
                                       : static_cast<i32>(node_to_joint[static_cast<usize>(&node - data.nodes)]),
                });
            }
            if (node.light != nullptr) {
                const cgltf_light &light = *node.light;
                glm::mat4 world{1.0f};
                cgltf_node_transform_world(&node, glm::value_ptr(world));
                const glm::vec3 color{light.color[0], light.color[1], light.color[2]};
                GltfLightInstance instance{
                    .name = label_from_name(light.name, "gltf_light"),
                    .radiance = color * (light.intensity * kPhotometricToRadiometric),
                    .world_transform = world,
                };
                switch (light.type) {
                    case cgltf_light_type_directional: instance.kind = GltfLightKind::Directional; break;
                    case cgltf_light_type_point: instance.kind = GltfLightKind::Point; break;
                    case cgltf_light_type_spot: instance.kind = GltfLightKind::Spot; break;
                    default: instance.kind = GltfLightKind::Point; break;
                }
                if (light.range > 0.0f) {
                    instance.range = light.range;
                }
                if (light.type == cgltf_light_type_spot) {
                    instance.inner_cone_cos = std::cos(light.spot_inner_cone_angle);
                    instance.outer_cone_cos = std::cos(light.spot_outer_cone_angle);
                }
                lights.push_back(instance);
            }
            for (cgltf_size i = 0; i < node.children_count; ++i) {
                collect_node_instances(data, *node.children[i], models, node_to_joint, instances, lights);
            }
        }


        using PendingMaterial = Detail::ImportMaterialValues;

    } // namespace

    /// Imports gltf using the supplied arguments and current state.
    ///
    /// @param assets `assets` value used by the operation.
    /// @param source Source value or resource.
    /// @param shader Shader used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::NotFound`, `AssetErrorCode::DecodeFailure`, `AssetErrorCode::IoFailure`, `AssetErrorCode::Unsupported`, `AssetErrorCode::InvalidDescription`.
    AssetExpected<GltfImportResult> import_gltf(AssetManager &assets, const std::filesystem::path &source,
                                                Asset shader, bool animations_only) {
        const Foundation::Stopwatch stopwatch;
        const std::string source_path = source.string();
        Foundation::log_info("GltfImport: loading '{}'...", source_path);

        cgltf_options options{};
        cgltf_data *raw_data = nullptr;
        cgltf_result result = cgltf_parse_file(&options, source_path.c_str(), &raw_data);
        if (result != cgltf_result_success) {
            return std::unexpected(gltf_error(
                result == cgltf_result_file_not_found ? AssetErrorCode::NotFound : AssetErrorCode::DecodeFailure,
                "Could not parse glTF asset '" + source_path + "': " + std::string{cgltf_result_message(result)} + ".",
                source));
        }
        const CgltfDataGuard guard{raw_data};
        cgltf_data &data = *raw_data;

        result = cgltf_load_buffers(&options, &data, source_path.c_str());
        if (result != cgltf_result_success) {
            return std::unexpected(gltf_error(
                AssetErrorCode::IoFailure,
                "Could not load glTF buffers for '" + source_path + "': " + std::string{cgltf_result_message(result)} +
                    ".",
                source));
        }

        result = cgltf_validate(&data);
        if (result != cgltf_result_success) {
            return std::unexpected(gltf_error(
                AssetErrorCode::DecodeFailure,
                "glTF asset '" + source_path + "' failed validation: " + std::string{cgltf_result_message(result)} +
                    ".",
                source));
        }

        const std::filesystem::path base_dir = source.parent_path();
        ImageCache image_cache{
            .entries = std::vector<std::array<std::array<std::optional<Asset>, 5>, 2>>(data.images_count)};


        std::vector<Asset> packed_texture_assets;


        std::optional<Asset> flat_normal_texture;
        const auto get_flat_normal_texture = [&]() -> AssetExpected<Asset> {
            if (flat_normal_texture) {
                return *flat_normal_texture;
            }
            AssetExpected<Asset> created = assets.create_texture(TextureAssetDesc{
                .width = 1,
                .height = 1,
                .color_space = TextureColorSpace::Linear,
                .pixels = std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}},
                .label = UString{"gltf flat normal"_ustr},
            });
            if (created) {
                flat_normal_texture = *created;
            }
            return created;
        };

        GltfImportResult out{};
        out.models.reserve(data.meshes_count);

        const auto rollback_models = [&assets, &out]() {
            for (Asset model : out.models) {
                (void)assets.unload(model);
            }
            out.models.clear();
        };

        std::vector<u32> scene_node_to_joint;
        std::vector<BuiltSkin> built_skins;
        built_skins.reserve(data.skins_count);
        for (cgltf_size i = 0; i < data.skins_count; ++i) {
            built_skins.push_back(build_skin(data, data.skins[i]));
        }
        // Weights address joints of the skin of the first node that instances the mesh.
        std::vector<i32> mesh_skin(data.meshes_count, -1);
        for (cgltf_size i = 0; i < data.nodes_count; ++i) {
            const cgltf_node &n = data.nodes[i];
            if (n.mesh != nullptr && n.skin != nullptr) {
                i32 &slot = mesh_skin[static_cast<usize>(n.mesh - data.meshes)];
                if (slot < 0) {
                    slot = static_cast<i32>(n.skin - data.skins);
                }
            }
        }

        for (cgltf_size mesh_index = 0; mesh_index < (animations_only ? 0 : data.meshes_count); ++mesh_index) {
            const cgltf_mesh &mesh = data.meshes[mesh_index];
            ModelAssetDesc desc{.label = label_from_name(mesh.name, "gltf_mesh")};
            desc.primitives.reserve(mesh.primitives_count);

            std::vector<PendingMaterial> pending;
            pending.reserve(mesh.primitives_count);

            bool primitive_failed = false;
            AssetError primitive_error{};

            for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
                const cgltf_primitive &primitive = mesh.primitives[primitive_index];


                if (primitive.type != cgltf_primitive_type_triangles) {
                    primitive_failed = true;
                    primitive_error = gltf_error(AssetErrorCode::Unsupported,
                                                 "glTF primitive mode is not TRIANGLES (only TRIANGLES is supported).",
                                                 source);
                    break;
                }

                const cgltf_accessor *position_accessor = nullptr;
                const cgltf_accessor *normal_accessor = nullptr;
                const cgltf_accessor *uv_accessor = nullptr;
                const cgltf_accessor *color_accessor = nullptr;
                const cgltf_accessor *tangent_accessor = nullptr;
                const cgltf_accessor *joints_accessor = nullptr;
                const cgltf_accessor *weights_accessor = nullptr;
                for (cgltf_size attr_index = 0; attr_index < primitive.attributes_count; ++attr_index) {
                    const cgltf_attribute &attribute = primitive.attributes[attr_index];
                    switch (attribute.type) {
                        case cgltf_attribute_type_position: position_accessor = attribute.data; break;
                        case cgltf_attribute_type_normal: normal_accessor = attribute.data; break;
                        case cgltf_attribute_type_texcoord:
                            if (attribute.index == 0) {
                                uv_accessor = attribute.data;
                            }
                            break;
                        case cgltf_attribute_type_color:
                            if (attribute.index == 0) {
                                color_accessor = attribute.data;
                            }
                            break;
                        case cgltf_attribute_type_tangent: tangent_accessor = attribute.data; break;
                        case cgltf_attribute_type_joints:
                            if (attribute.index == 0) {
                                joints_accessor = attribute.data;
                            }
                            break;
                        case cgltf_attribute_type_weights:
                            if (attribute.index == 0) {
                                weights_accessor = attribute.data;
                            }
                            break;
                        default: break;
                    }
                }
                if (position_accessor == nullptr) {
                    primitive_failed = true;
                    primitive_error = gltf_error(AssetErrorCode::InvalidDescription,
                                                 "glTF primitive has no POSITION attribute.", source);
                    break;
                }

                const auto vertex_count = static_cast<usize>(position_accessor->count);
                std::vector<f32> positions(vertex_count * 3);
                cgltf_accessor_unpack_floats(position_accessor, positions.data(), positions.size());

                std::vector<f32> normals;
                if (normal_accessor != nullptr) {
                    normals.resize(vertex_count * 3);
                    cgltf_accessor_unpack_floats(normal_accessor, normals.data(), normals.size());
                }

                std::vector<f32> uvs;
                if (uv_accessor != nullptr) {
                    uvs.resize(vertex_count * 2);
                    cgltf_accessor_unpack_floats(uv_accessor, uvs.data(), uvs.size());
                }

                usize color_components = 0;
                std::vector<f32> colors;
                if (color_accessor != nullptr) {
                    color_components = cgltf_num_components(color_accessor->type);
                    colors.resize(vertex_count * color_components);
                    cgltf_accessor_unpack_floats(color_accessor, colors.data(), colors.size());
                }

                std::vector<f32> tangents;
                if (tangent_accessor != nullptr) {
                    tangents.resize(vertex_count * 4);
                    cgltf_accessor_unpack_floats(tangent_accessor, tangents.data(), tangents.size());
                }

                std::vector<Renderer::GeometryVertex> vertices(vertex_count);
                for (usize v = 0; v < vertex_count; ++v) {
                    Renderer::GeometryVertex &vertex = vertices[v];
                    vertex.position = {positions[v * 3 + 0], positions[v * 3 + 1], positions[v * 3 + 2]};
                    if (!normals.empty()) {
                        vertex.normal = {normals[v * 3 + 0], normals[v * 3 + 1], normals[v * 3 + 2]};
                    }
                    if (!uvs.empty()) {
                        vertex.uv = {uvs[v * 2 + 0], uvs[v * 2 + 1]};
                    }
                    if (!colors.empty()) {
                        vertex.color = glm::vec4{
                            colors[v * color_components + 0],
                            colors[v * color_components + 1],
                            colors[v * color_components + 2],
                            color_components == 4 ? colors[v * color_components + 3] : 1.0f,
                        };
                    }
                    if (!tangents.empty()) {
                        vertex.tangent = {
                            tangents[v * 4 + 0], tangents[v * 4 + 1], tangents[v * 4 + 2], tangents[v * 4 + 3],
                        };
                    }
                }

                std::vector<u32> indices;
                if (primitive.indices != nullptr) {
                    indices.resize(primitive.indices->count);
                    for (usize i = 0; i < indices.size(); ++i) {
                        indices[i] = static_cast<u32>(cgltf_accessor_read_index(primitive.indices, i));
                    }
                } else {
                    indices.resize(vertex_count);
                    for (usize i = 0; i < vertex_count; ++i) {
                        indices[i] = static_cast<u32>(i);
                    }
                }


                if (normal_accessor == nullptr) {
                    Detail::generate_normals(vertices, indices);
                }


                if (tangent_accessor == nullptr && !uvs.empty()) {
                    Detail::generate_tangents(vertices, indices);
                }

                const std::string primitive_label =
                    desc.label.cpp_string() + " primitive " + std::to_string(primitive_index);
                ModelPrimitiveDesc primitive_desc{
                    .mesh = Renderer::Mesh::from_vertices(vertices, indices, primitive_label.c_str()),
                    .shader = shader,
                    .double_sided = primitive.material != nullptr && primitive.material->double_sided,
                };
                if (joints_accessor != nullptr && weights_accessor != nullptr && mesh_skin[mesh_index] >= 0 &&
                    joints_accessor->count == vertex_count && weights_accessor->count == vertex_count) {
                    const auto &joint_map = built_skins[static_cast<usize>(mesh_skin[mesh_index])].skin_joint_to_skeleton;
                    auto skin_weights = std::make_shared<Animation::SkinWeights>();
                    skin_weights->joints.resize(vertex_count);
                    skin_weights->weights.resize(vertex_count);
                    for (usize v = 0; v < vertex_count; ++v) {
                        cgltf_uint joint_values[4]{};
                        f32 weight_values[4]{};
                        cgltf_accessor_read_uint(joints_accessor, v, joint_values, 4);
                        cgltf_accessor_read_float(weights_accessor, v, weight_values, 4);
                        for (int k = 0; k < 4; ++k) {
                            skin_weights->joints[v][k] =
                                joint_values[k] < joint_map.size() ? joint_map[joint_values[k]] : 0u;
                            skin_weights->weights[v][k] = weight_values[k];
                        }
                    }
                    primitive_desc.skin = std::move(skin_weights);
                }
                if (primitive.targets_count > 0) {
                    Animation::MorphBuilder morph_builder(vertex_count);
                    for (cgltf_size t = 0; t < primitive.targets_count; ++t) {
                        const cgltf_accessor *position_target = nullptr;
                        const cgltf_accessor *normal_target = nullptr;
                        for (cgltf_size ai = 0; ai < primitive.targets[t].attributes_count; ++ai) {
                            const cgltf_attribute &attribute = primitive.targets[t].attributes[ai];
                            if (attribute.type == cgltf_attribute_type_position) position_target = attribute.data;
                            if (attribute.type == cgltf_attribute_type_normal) normal_target = attribute.data;
                        }
                        const auto unpack_vec3 = [vertex_count](const cgltf_accessor *accessor) {
                            std::vector<glm::vec3> out(vertex_count, glm::vec3{0.0f});
                            if (accessor != nullptr && accessor->count == vertex_count) {
                                std::vector<f32> flat(vertex_count * 3);
                                cgltf_accessor_unpack_floats(accessor, flat.data(), flat.size());
                                for (usize v = 0; v < vertex_count; ++v) {
                                    out[v] = {flat[v * 3], flat[v * 3 + 1], flat[v * 3 + 2]};
                                }
                            }
                            return out;
                        };
                        const std::string name = t < mesh.target_names_count && mesh.target_names[t] != nullptr
                                                     ? mesh.target_names[t]
                                                     : "target_" + std::to_string(t);
                        morph_builder.add_target(name, unpack_vec3(position_target),
                                                 normal_target != nullptr ? unpack_vec3(normal_target)
                                                                          : std::vector<glm::vec3>{},
                                                 t < mesh.weights_count ? mesh.weights[t] : 0.0f);
                    }
                    primitive_desc.morph = std::make_shared<const Animation::MorphTargetSet>(morph_builder.build());
                }

                PendingMaterial material_values{};
                if (const cgltf_material *material = primitive.material; material != nullptr) {
                    const cgltf_texture *mr_gltf_texture =
                        (material->has_pbr_metallic_roughness &&
                         material->pbr_metallic_roughness.metallic_roughness_texture.texture != nullptr &&
                         material->pbr_metallic_roughness.metallic_roughness_texture.texture->image != nullptr)
                            ? material->pbr_metallic_roughness.metallic_roughness_texture.texture
                            : nullptr;
                    const cgltf_texture *occlusion_gltf_texture =
                        (material->occlusion_texture.texture != nullptr &&
                         material->occlusion_texture.texture->image != nullptr)
                            ? material->occlusion_texture.texture
                            : nullptr;


                    std::optional<Asset> packed_orm_texture;
                    if (mr_gltf_texture != nullptr && occlusion_gltf_texture != nullptr &&
                        mr_gltf_texture->image != occlusion_gltf_texture->image) {
                        AssetExpected<Detail::DecodedImage> occlusion_pixels =
                            decode_gltf_image_pixels(*occlusion_gltf_texture->image, base_dir);
                        AssetExpected<Detail::DecodedImage> mr_pixels =
                            decode_gltf_image_pixels(*mr_gltf_texture->image, base_dir);
                        if (occlusion_pixels && mr_pixels && occlusion_pixels->width == mr_pixels->width &&
                            occlusion_pixels->height == mr_pixels->height) {
                            AssetExpected<Asset> orm_asset = assets.create_orm_texture(
                                occlusion_pixels->pixels(), mr_pixels->pixels(), occlusion_pixels->width,
                                occlusion_pixels->height, UString{"gltf orm"_ustr});
                            if (orm_asset) {
                                packed_orm_texture = *orm_asset;
                                packed_texture_assets.push_back(*orm_asset);
                            }
                        }
                    }


                    const bool occlusion_shares_mr_image =
                        mr_gltf_texture != nullptr && occlusion_gltf_texture != nullptr &&
                        mr_gltf_texture->image == occlusion_gltf_texture->image;

                    if (material->has_pbr_metallic_roughness) {
                        const cgltf_pbr_metallic_roughness &pbr = material->pbr_metallic_roughness;
                        material_values.base_color_factor = glm::vec4{
                            pbr.base_color_factor[0],
                            pbr.base_color_factor[1],
                            pbr.base_color_factor[2],
                            pbr.base_color_factor[3],
                        };
                        material_values.metallic_factor = pbr.metallic_factor;
                        material_values.roughness_factor = pbr.roughness_factor;

                        if (const cgltf_texture *texture = pbr.base_color_texture.texture;
                            texture != nullptr && texture->image != nullptr) {
                            const auto image_index = static_cast<usize>(texture->image - data.images);


                            const TextureKind base_color_kind = material->alpha_mode == cgltf_alpha_mode_opaque
                                ? TextureKind::ColorOpaque
                                : TextureKind::ColorAlpha;
                            AssetExpected<Asset> base_color_texture = load_image(
                                assets, *texture->image, image_index, TextureColorSpace::Srgb, base_color_kind,
                                base_dir, image_cache);
                            if (!base_color_texture) {
                                primitive_failed = true;
                                primitive_error = base_color_texture.error();
                                break;
                            }
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"base_color_texture"_ustr},
                                .texture = *base_color_texture,
                            });
                        }

                        if (packed_orm_texture) {
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"metallic_roughness_texture"_ustr},
                                .texture = *packed_orm_texture,
                            });
                        } else if (mr_gltf_texture != nullptr) {
                            const auto image_index = static_cast<usize>(mr_gltf_texture->image - data.images);


                            std::optional<Asset> mr_texture_bc5;
                            if (AssetExpected<Detail::DecodedImage> mr_pixels =
                                    decode_gltf_image_pixels(*mr_gltf_texture->image, base_dir)) {
                                if (auto repacked = Detail::pack_metallic_roughness_rg(
                                        mr_pixels->pixels(), mr_pixels->width, mr_pixels->height)) {
                                    AssetExpected<Asset> created = assets.create_texture(TextureAssetDesc{
                                        .width = mr_pixels->width,
                                        .height = mr_pixels->height,
                                        .color_space = TextureColorSpace::Linear,
                                        .pixels = std::move(*repacked),
                                        .label = UString{"gltf metallic_roughness (bc5)"_ustr},
                                        .kind = TextureKind::MetallicRoughness,
                                    });
                                    if (created) {
                                        mr_texture_bc5 = *created;
                                        packed_texture_assets.push_back(*created);
                                    }
                                }
                            }

                            Asset mr_texture_asset;
                            if (mr_texture_bc5) {
                                mr_texture_asset = *mr_texture_bc5;
                                material_values.metallic_roughness_channels_rg = 1.0f;
                            } else {


                                AssetExpected<Asset> mr_texture = load_image(
                                    assets, *mr_gltf_texture->image, image_index, TextureColorSpace::Linear,
                                    TextureKind::ColorAlpha, base_dir, image_cache);
                                if (!mr_texture) {
                                    primitive_failed = true;
                                    primitive_error = mr_texture.error();
                                    break;
                                }
                                mr_texture_asset = *mr_texture;
                            }
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"metallic_roughness_texture"_ustr},
                                .texture = mr_texture_asset,
                            });
                        }
                    }

                    if (material->has_specular) {
                        material_values.specular_factor = material->specular.specular_factor;
                    }
                    if (material->has_ior) {
                        material_values.ior = material->ior.ior;
                    }
                    if (material->alpha_mode == cgltf_alpha_mode_mask) {
                        material_values.alpha_cutoff = material->alpha_cutoff;
                    }

                    material_values.emissive_factor = glm::vec4{
                        material->emissive_factor[0],
                        material->emissive_factor[1],
                        material->emissive_factor[2],
                        0.0f,
                    };
                    if (material->has_emissive_strength) {
                        material_values.emissive_strength = material->emissive_strength.emissive_strength;
                    }

                    if (occlusion_gltf_texture != nullptr) {
                        material_values.occlusion_strength = material->occlusion_texture.scale;
                        if (packed_orm_texture) {
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"occlusion_texture"_ustr},
                                .texture = *packed_orm_texture,
                            });
                        } else {
                            const auto image_index = static_cast<usize>(occlusion_gltf_texture->image - data.images);


                            AssetExpected<Asset> occlusion_texture = load_image(
                                assets, *occlusion_gltf_texture->image, image_index, TextureColorSpace::Linear,
                                occlusion_shares_mr_image ? TextureKind::ColorAlpha : TextureKind::Mask, base_dir,
                                image_cache);
                            if (!occlusion_texture) {
                                primitive_failed = true;
                                primitive_error = occlusion_texture.error();
                                break;
                            }
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"occlusion_texture"_ustr},
                                .texture = *occlusion_texture,
                            });
                        }
                    }

                    if (const cgltf_texture *texture = material->emissive_texture.texture;
                        texture != nullptr && texture->image != nullptr) {
                        const auto image_index = static_cast<usize>(texture->image - data.images);
                        AssetExpected<Asset> emissive_texture = load_image(
                            assets, *texture->image, image_index, TextureColorSpace::Srgb,
                            TextureKind::ColorAlpha, base_dir, image_cache);
                        if (!emissive_texture) {
                            primitive_failed = true;
                            primitive_error = emissive_texture.error();
                            break;
                        }
                        primitive_desc.textures.push_back(ModelTextureBinding{
                            .slot = UString{"emissive_texture"_ustr},
                            .texture = *emissive_texture,
                        });
                    }
                }


                AssetExpected<Asset> normal_texture = [&]() -> AssetExpected<Asset> {
                    if (const cgltf_texture *texture = primitive.material != nullptr
                                                            ? primitive.material->normal_texture.texture
                                                            : nullptr;
                        texture != nullptr && texture->image != nullptr) {
                        const auto image_index = static_cast<usize>(texture->image - data.images);


                        return load_image(
                            assets, *texture->image, image_index, TextureColorSpace::Linear,
                            TextureKind::NormalMap, base_dir, image_cache);
                    }
                    return get_flat_normal_texture();
                }();
                if (!normal_texture) {
                    primitive_failed = true;
                    primitive_error = normal_texture.error();
                    break;
                }
                primitive_desc.textures.push_back(ModelTextureBinding{
                    .slot = UString{"normal_texture"_ustr},
                    .texture = *normal_texture,
                });

                if (primitive_failed) {
                    break;
                }

                desc.primitives.push_back(std::move(primitive_desc));
                pending.push_back(material_values);
            }

            if (primitive_failed) {
                rollback_models();
                return std::unexpected(primitive_error);
            }

            AssetExpected<Asset> model = assets.create_model(std::move(desc));
            if (!model) {
                rollback_models();
                return std::unexpected(model.error());
            }

            for (usize primitive_index = 0; primitive_index < pending.size(); ++primitive_index) {
                if (AssetResult set = Detail::apply_material_values(assets, *model, primitive_index, pending[primitive_index]); !set) {
                    (void)assets.unload(*model);
                    rollback_models();
                    return std::unexpected(set.error());
                }
            }

            out.models.push_back(*model);
        }

        {
            SceneRig rig = build_scene_rig(data);
            out.scene_skeleton = std::move(rig.skeleton);
            out.scene_clips = std::move(rig.clips);
            scene_node_to_joint = std::move(rig.node_to_joint);
        }
        out.skins.reserve(built_skins.size());
        for (BuiltSkin &built : built_skins) {
            out.skins.push_back(std::move(built.skin));
        }

        const cgltf_scene *scene =
            data.scene != nullptr ? data.scene : (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
        if (scene != nullptr) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                collect_node_instances(data, *scene->nodes[i], out.models, scene_node_to_joint, out.instances, out.lights);
            }
        } else {
            for (cgltf_size i = 0; i < data.nodes_count; ++i) {
                if (data.nodes[i].parent == nullptr) {
                    collect_node_instances(data, data.nodes[i], out.models, scene_node_to_joint, out.instances, out.lights);
                }
            }
        }


        {
            usize total_triangles = 0;
            usize total_mesh_bytes = 0;
            for (Asset model : out.models) {
                if (auto model_info = assets.model_info(model)) {
                    total_triangles += model_info->triangle_count;
                }
                if (auto asset_info = assets.info(model)) {
                    total_mesh_bytes += asset_info->memory_bytes;
                }
            }
            usize total_texture_bytes = 0;
            for (const std::array<std::array<std::optional<Asset>, 5>, 2> &color_space_entry : image_cache.entries) {
                for (const std::array<std::optional<Asset>, 5> &kind_entry : color_space_entry) {
                    for (const std::optional<Asset> &texture : kind_entry) {
                        if (texture) {
                            if (auto texture_info = assets.info(*texture)) {
                                total_texture_bytes += texture_info->memory_bytes;
                            }
                        }
                    }
                }
            }
            if (flat_normal_texture) {
                if (auto texture_info = assets.info(*flat_normal_texture)) {
                    total_texture_bytes += texture_info->memory_bytes;
                }
            }
            for (Asset texture : packed_texture_assets) {
                if (auto texture_info = assets.info(texture)) {
                    total_texture_bytes += texture_info->memory_bytes;
                }
            }
            const f64 mesh_mb = static_cast<f64>(total_mesh_bytes) / (1024.0 * 1024.0);
            const f64 texture_mb = static_cast<f64>(total_texture_bytes) / (1024.0 * 1024.0);
            Foundation::log_info(
                "Loaded glTF '{}': {} model(s), {} triangles, ~{:.2f} MB VRAM ({:.2f} MB mesh, {:.2f} MB textures) in {}",
                source_path, out.models.size(), total_triangles, mesh_mb + texture_mb, mesh_mb, texture_mb,
                stopwatch.elapsed_human());
        }

        return out;
    }

} // namespace SFT::Engine
