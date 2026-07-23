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

#include "GltfImport.hpp"

#include "AssetManager.hpp"

#include <Renderer/Mesh.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] AssetError gltf_error(AssetErrorCode code, std::string message,
                                            std::filesystem::path source = {}) {
            return AssetError{.code = code, .message = UString{message}, .source = std::move(source)};
        }

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
            ~CgltfDataGuard() {
                if (data != nullptr) {
                    cgltf_free(data);
                }
            }
        };

        // image_index -> {srgb, linear} cached texture assets. A glTF image referenced from both an
        // sRGB-decoded slot (base color) and a linear-data slot (metallic/roughness) would otherwise
        // upload the same bytes twice with two different color-space interpretations, so the cache
        // key includes color space, not just the image index.
        struct ImageCache {
            std::vector<std::array<std::optional<Asset>, 2>> entries;
        };

        [[nodiscard]] usize color_space_slot(TextureColorSpace color_space) noexcept {
            return color_space == TextureColorSpace::Srgb ? 0 : 1;
        }

        [[nodiscard]] AssetExpected<Asset> load_image(
            AssetManager &assets,
            const cgltf_image &image,
            usize image_index,
            TextureColorSpace color_space,
            const std::filesystem::path &base_dir,
            ImageCache &cache) {
            const usize slot = color_space_slot(color_space);
            if (std::optional<Asset> &cached = cache.entries[image_index][slot]; cached) {
                return *cached;
            }

            const UString label = label_from_name(image.name, "gltf_image");

            AssetExpected<Asset> loaded = [&]() -> AssetExpected<Asset> {
                if (image.buffer_view != nullptr) {
                    const std::uint8_t *bytes = cgltf_buffer_view_data(image.buffer_view);
                    if (bytes == nullptr) {
                        return std::unexpected(gltf_error(AssetErrorCode::DecodeFailure,
                                                           "glTF embedded image has no backing buffer data."));
                    }
                    return assets.create_texture_from_encoded_bytes(
                        std::span<const std::byte>{
                            reinterpret_cast<const std::byte *>(bytes),
                            static_cast<usize>(image.buffer_view->size)},
                        color_space,
                        label);
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
                    AssetExpected<Asset> texture = assets.create_texture_from_encoded_bytes(
                        std::span<const std::byte>{reinterpret_cast<const std::byte *>(decoded), decoded_size},
                        color_space,
                        label);
                    std::free(decoded);
                    return texture;
                }

                std::string decoded_uri = image.uri;
                const cgltf_size decoded_length = cgltf_decode_uri(decoded_uri.data());
                decoded_uri.resize(decoded_length);
                return assets.load_texture(base_dir / decoded_uri, color_space, label);
            }();

            if (loaded) {
                cache.entries[image_index][slot] = *loaded;
            }
            return loaded;
        }

        // glTF punctual lights are photometric (lux for directional, candela for point/spot); this
        // is the standard lm/W factor (also used by Filament and the Khronos glTF sample viewer) to
        // land in this engine's own ad hoc radiometric radiance scale.
        constexpr f32 kPhotometricToRadiometric = 1.0f / 683.0f;

        void collect_node_instances(
            const cgltf_data &data,
            const cgltf_node &node,
            const std::vector<Asset> &models,
            std::vector<GltfNodeInstance> &instances,
            std::vector<GltfLightInstance> &lights) {
            if (node.mesh != nullptr) {
                const auto mesh_index = static_cast<usize>(node.mesh - data.meshes);
                glm::mat4 world{1.0f};
                cgltf_node_transform_world(&node, glm::value_ptr(world));
                instances.push_back(GltfNodeInstance{
                    .name = label_from_name(node.name, "gltf_node"),
                    .model = models[mesh_index],
                    .world_transform = world,
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
                collect_node_instances(data, *node.children[i], models, instances, lights);
            }
        }

        // Deferred per-primitive glTF-spec-default parameter values, applied after create_model()
        // returns a material instance — ModelPrimitiveDesc has no generic float/vec4 parameter slots,
        // only vertex_color/textures (Engine/AssetManager.hpp), and every MaterialInstance parameter
        // is otherwise zero-initialized (plans/pbr-material-system.md).
        struct PendingMaterial {
            glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
            f32 metallic_factor = 1.0f;
            f32 roughness_factor = 1.0f;
            f32 specular_factor = 1.0f;
            f32 ior = 1.5f;
            // 0.0 never discards (glTF OPAQUE, and this struct's own default). MASK sets the glTF
            // material's real alphaCutoff; BLEND is treated as opaque — see gbuffer_geometry.slang's
            // alpha_cutoff comment for why true blending isn't supported by this deferred pass yet.
            f32 alpha_cutoff = 0.0f;
        };

    } // namespace

    AssetExpected<GltfImportResult> import_gltf(AssetManager &assets, const std::filesystem::path &source,
                                                Asset shader) {
        const std::string source_path = source.string();

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
        ImageCache image_cache{.entries = std::vector<std::array<std::optional<Asset>, 2>>(data.images_count)};

        // Lazily created once and shared by every material with no real normalTexture: a flat
        // (128, 128, 255) tangent-space normal (unpacks to (0, 0, 1), a no-op perturbation) — bound
        // explicitly rather than relying on Renderer's own default-white-texture fallback, which
        // would otherwise decode as tangent-space (1, 1, 1), a non-unit, wrong-pointing vector.
        std::optional<Asset> flat_normal_texture;
        const auto get_flat_normal_texture = [&]() -> AssetExpected<Asset> {
            if (flat_normal_texture) {
                return *flat_normal_texture;
            }
            AssetExpected<Asset> created = assets.create_texture(TextureAssetDesc{
                .width = 1,
                .height = 1,
                .color_space = TextureColorSpace::Linear,
                .rgba8 = std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}},
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

        for (cgltf_size mesh_index = 0; mesh_index < data.meshes_count; ++mesh_index) {
            const cgltf_mesh &mesh = data.meshes[mesh_index];
            ModelAssetDesc desc{.label = label_from_name(mesh.name, "gltf_mesh")};
            desc.primitives.reserve(mesh.primitives_count);

            std::vector<PendingMaterial> pending;
            pending.reserve(mesh.primitives_count);

            bool primitive_failed = false;
            AssetError primitive_error{};

            for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
                const cgltf_primitive &primitive = mesh.primitives[primitive_index];
                // Only triangle-list primitives are handled — every Khronos glTF-Sample-Assets model
                // uses TRIANGLES, and the other five modes (points/lines/strip/fan variants) are rare
                // enough in real content to not be worth the extra triangulation logic here.
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

                // makeSurfaceBasis()-derived shading only needs a plausible smooth normal, not the
                // source mesh's authored one — synthesize via face-normal accumulation when the glTF
                // primitive omits NORMAL (legal per spec; renderers are expected to compute one).
                if (normal_accessor == nullptr) {
                    std::vector<glm::vec3> accumulated(vertex_count, glm::vec3{0.0f});
                    for (usize i = 0; i + 2 < indices.size(); i += 3) {
                        const u32 a = indices[i];
                        const u32 b = indices[i + 1];
                        const u32 c = indices[i + 2];
                        const glm::vec3 face_normal = glm::cross(
                            vertices[b].position - vertices[a].position,
                            vertices[c].position - vertices[a].position);
                        accumulated[a] += face_normal;
                        accumulated[b] += face_normal;
                        accumulated[c] += face_normal;
                    }
                    for (usize v = 0; v < vertex_count; ++v) {
                        const f32 length = glm::length(accumulated[v]);
                        vertices[v].normal = length > 1e-8f ? accumulated[v] / length : glm::vec3{0.0f, 1.0f, 0.0f};
                    }
                }

                // Standard UV-gradient tangent generation (Lengyel's method) when the primitive omits
                // TANGENT (legal per spec) but has UVs to derive one from — needed for correct normal
                // mapping (gbuffer_geometry.slang's TBN basis). Skipped without UVs: there's no
                // meaningful tangent direction to compute, and the vertex's default {1,0,0,1} is only
                // wrong if a normal map is actually sampled, which a UV-less primitive can't do anyway.
                if (tangent_accessor == nullptr && !uvs.empty()) {
                    std::vector<glm::vec3> tan_accum(vertex_count, glm::vec3{0.0f});
                    std::vector<glm::vec3> bitan_accum(vertex_count, glm::vec3{0.0f});
                    for (usize i = 0; i + 2 < indices.size(); i += 3) {
                        const u32 a = indices[i];
                        const u32 b = indices[i + 1];
                        const u32 c = indices[i + 2];
                        const glm::vec3 edge1 = vertices[b].position - vertices[a].position;
                        const glm::vec3 edge2 = vertices[c].position - vertices[a].position;
                        const glm::vec2 delta_uv1 = vertices[b].uv - vertices[a].uv;
                        const glm::vec2 delta_uv2 = vertices[c].uv - vertices[a].uv;
                        const f32 determinant = delta_uv1.x * delta_uv2.y - delta_uv2.x * delta_uv1.y;
                        if (std::abs(determinant) < 1e-12f) {
                            continue;
                        }
                        const f32 inv_determinant = 1.0f / determinant;
                        const glm::vec3 tangent = inv_determinant * (delta_uv2.y * edge1 - delta_uv1.y * edge2);
                        const glm::vec3 bitangent = inv_determinant * (delta_uv1.x * edge2 - delta_uv2.x * edge1);
                        tan_accum[a] += tangent; tan_accum[b] += tangent; tan_accum[c] += tangent;
                        bitan_accum[a] += bitangent; bitan_accum[b] += bitangent; bitan_accum[c] += bitangent;
                    }
                    for (usize v = 0; v < vertex_count; ++v) {
                        const glm::vec3 &normal = vertices[v].normal;
                        glm::vec3 tangent = tan_accum[v] - normal * glm::dot(normal, tan_accum[v]);
                        const f32 length = glm::length(tangent);
                        if (length > 1e-8f) {
                            tangent /= length;
                        } else {
                            const glm::vec3 reference = std::abs(normal.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f}
                                                                                 : glm::vec3{0.0f, 1.0f, 0.0f};
                            tangent = glm::normalize(glm::cross(reference, normal));
                        }
                        const f32 handedness = glm::dot(glm::cross(normal, tangent), bitan_accum[v]) < 0.0f ? -1.0f : 1.0f;
                        vertices[v].tangent = glm::vec4{tangent, handedness};
                    }
                }

                const std::string primitive_label =
                    desc.label.cpp_string() + " primitive " + std::to_string(primitive_index);
                ModelPrimitiveDesc primitive_desc{
                    .mesh = Renderer::Mesh::from_vertices(vertices, indices, primitive_label.c_str()),
                    .shader = shader,
                };

                PendingMaterial material_values{};
                if (const cgltf_material *material = primitive.material; material != nullptr) {
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
                            AssetExpected<Asset> base_color_texture = load_image(
                                assets, *texture->image, image_index, TextureColorSpace::Srgb, base_dir, image_cache);
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
                        if (const cgltf_texture *texture = pbr.metallic_roughness_texture.texture;
                            texture != nullptr && texture->image != nullptr) {
                            const auto image_index = static_cast<usize>(texture->image - data.images);
                            // glTF packs roughness/metallic as data (G/B channels), not display color —
                            // decoding it sRGB would corrupt every value that isn't 0 or 1.
                            AssetExpected<Asset> mr_texture = load_image(
                                assets, *texture->image, image_index, TextureColorSpace::Linear, base_dir,
                                image_cache);
                            if (!mr_texture) {
                                primitive_failed = true;
                                primitive_error = mr_texture.error();
                                break;
                            }
                            primitive_desc.textures.push_back(ModelTextureBinding{
                                .slot = UString{"metallic_roughness_texture"_ustr},
                                .texture = *mr_texture,
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
                }

                // Bound unconditionally (even with no material at all, i.e. glTF's default material)
                // since every material instance always has some texture in this slot regardless —
                // Renderer::initialize_material_instance_state falls back to a plain white texture,
                // which would be wrong here (see get_flat_normal_texture's doc comment).
                AssetExpected<Asset> normal_texture = [&]() -> AssetExpected<Asset> {
                    if (const cgltf_texture *texture = primitive.material != nullptr
                                                            ? primitive.material->normal_texture.texture
                                                            : nullptr;
                        texture != nullptr && texture->image != nullptr) {
                        const auto image_index = static_cast<usize>(texture->image - data.images);
                        // Normal maps are data (tangent-space directions), not display color —
                        // sRGB-decoding one would corrupt every direction that isn't axis-aligned.
                        return load_image(
                            assets, *texture->image, image_index, TextureColorSpace::Linear, base_dir,
                            image_cache);
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
                const PendingMaterial &values = pending[primitive_index];
                AssetResult set = assets.set_model_vec4(*model, primitive_index, "base_color_factor",
                                                        values.base_color_factor);
                if (set) {
                    set = assets.set_model_float(*model, primitive_index, "metallic_factor", values.metallic_factor);
                }
                if (set) {
                    set = assets.set_model_float(*model, primitive_index, "roughness_factor", values.roughness_factor);
                }
                if (set) {
                    set = assets.set_model_float(*model, primitive_index, "specular_factor", values.specular_factor);
                }
                if (set) {
                    set = assets.set_model_float(*model, primitive_index, "ior", values.ior);
                }
                if (set) {
                    set = assets.set_model_float(*model, primitive_index, "alpha_cutoff", values.alpha_cutoff);
                }
                if (!set) {
                    (void)assets.unload(*model);
                    rollback_models();
                    return std::unexpected(set.error());
                }
            }

            out.models.push_back(*model);
        }

        const cgltf_scene *scene =
            data.scene != nullptr ? data.scene : (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
        if (scene != nullptr) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                collect_node_instances(data, *scene->nodes[i], out.models, out.instances, out.lights);
            }
        } else {
            for (cgltf_size i = 0; i < data.nodes_count; ++i) {
                if (data.nodes[i].parent == nullptr) {
                    collect_node_instances(data, data.nodes[i], out.models, out.instances, out.lights);
                }
            }
        }

        return out;
    }

} // namespace SFT::Engine
