#include <Engine/FbxImport.hpp>

#include <Engine/AssetManager.hpp>
#include <Engine/ImportCommon.hpp>

#include <Animation/Morph.hpp>

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] AssetError fbx_error(AssetErrorCode code, std::string message, std::filesystem::path source) {
            return AssetError{.code = code, .message = UString{message}, .source = std::move(source)};
        }

        [[nodiscard]] std::string to_std(ufbx_string s) { return std::string(s.data, s.length); }
        [[nodiscard]] glm::vec3 to_glm(ufbx_vec3 v) { return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)}; }
        [[nodiscard]] glm::vec2 to_glm(ufbx_vec2 v) { return {static_cast<f32>(v.x), static_cast<f32>(v.y)}; }
        [[nodiscard]] glm::vec4 to_glm(ufbx_vec4 v) {
            return {static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z), static_cast<f32>(v.w)};
        }
        [[nodiscard]] glm::mat4 to_glm(const ufbx_matrix &m) {
            return glm::mat4(glm::vec4(to_glm(m.cols[0]), 0.0f), glm::vec4(to_glm(m.cols[1]), 0.0f),
                             glm::vec4(to_glm(m.cols[2]), 0.0f), glm::vec4(to_glm(m.cols[3]), 1.0f));
        }

        struct SceneDeleter {
            void operator()(ufbx_scene *s) const noexcept { ufbx_free_scene(s); }
        };
        struct BakeDeleter {
            void operator()(ufbx_baked_anim *b) const noexcept { ufbx_free_baked_anim(b); }
        };

        void append_vec3_keys(Animation::Track &track, const ufbx_baked_vec3_list &keys, double origin, bool constant) {
            track.interpolation = Animation::Interpolation::Linear;
            const usize count = constant ? std::min<usize>(1, keys.count) : keys.count;
            for (usize i = 0; i < count; ++i) {
                track.times.push_back(constant ? 0.0f : static_cast<f32>(std::max(0.0, keys.data[i].time - origin)));
                track.values.insert(track.values.end(), {static_cast<f32>(keys.data[i].value.x),
                                                         static_cast<f32>(keys.data[i].value.y),
                                                         static_cast<f32>(keys.data[i].value.z)});
            }
        }
        void append_quat_keys(Animation::Track &track, const ufbx_baked_quat_list &keys, double origin, bool constant) {
            track.interpolation = Animation::Interpolation::Linear;
            const usize count = constant ? std::min<usize>(1, keys.count) : keys.count;
            for (usize i = 0; i < count; ++i) {
                const ufbx_quat q = keys.data[i].value;
                track.times.push_back(constant ? 0.0f : static_cast<f32>(std::max(0.0, keys.data[i].time - origin)));
                track.values.insert(track.values.end(), {static_cast<f32>(q.x), static_cast<f32>(q.y),
                                                         static_cast<f32>(q.z), static_cast<f32>(q.w)});
            }
        }

        struct VertexKey {
            u32 position, normal, uv, color;
            bool operator==(const VertexKey &o) const noexcept {
                return position == o.position && normal == o.normal && uv == o.uv && color == o.color;
            }
        };
        struct VertexKeyHash {
            usize operator()(const VertexKey &k) const noexcept {
                usize h = k.position * 0x9E3779B1u;
                h ^= (k.normal + 0x7F4A7C15u) + (h << 6) + (h >> 2);
                h ^= (k.uv + 0x165667B1u) + (h << 6) + (h >> 2);
                h ^= (k.color + 0x27D4EB2Fu) + (h << 6) + (h >> 2);
                return h;
            }
        };

    } // namespace

    AssetExpected<GltfImportResult> import_fbx(AssetManager &assets, const std::filesystem::path &source, Asset shader,
                                               bool animations_only) {
        ufbx_load_opts opts{};
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0;
        opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
        opts.generate_missing_normals = true;
        opts.clean_skin_weights = true;
        opts.load_external_files = true;
        opts.ignore_missing_external_files = true;
        opts.ignore_geometry = animations_only;

        ufbx_error error{};
        std::unique_ptr<ufbx_scene, SceneDeleter> scene{ufbx_load_file(source.string().c_str(), &opts, &error)};
        if (!scene) {
            return std::unexpected(fbx_error(AssetErrorCode::DecodeFailure,
                                             "ufbx could not load the file: " + to_std(error.description), source));
        }

        GltfImportResult out;
        const std::filesystem::path base_dir = source.parent_path();

        // ---- the node hierarchy as a parent-first skeleton -------------------------------------
        const usize node_count = scene->nodes.count;
        std::vector<ufbx_node *> ordered(scene->nodes.data, scene->nodes.data + node_count);
        const auto depth = [](const ufbx_node *n) {
            usize d = 0;
            for (; n->parent != nullptr; n = n->parent) ++d;
            return d;
        };
        std::stable_sort(ordered.begin(), ordered.end(),
                         [&depth](const ufbx_node *a, const ufbx_node *b) { return depth(a) < depth(b); });
        std::vector<u32> node_to_joint(node_count, Animation::no_joint);
        for (usize j = 0; j < ordered.size(); ++j) node_to_joint[ordered[j]->typed_id] = static_cast<u32>(j);

        auto skeleton = std::make_shared<Animation::Skeleton>();
        for (const ufbx_node *node : ordered) {
            skeleton->names.push_back(node->name.length > 0 ? to_std(node->name) : "node");
            skeleton->parents.push_back(node->parent != nullptr ? node_to_joint[node->parent->typed_id] : Animation::no_joint);
            Animation::JointTransform t;
            t.translation = to_glm(node->local_transform.translation);
            t.rotation = glm::normalize(glm::quat(static_cast<f32>(node->local_transform.rotation.w),
                                                  static_cast<f32>(node->local_transform.rotation.x),
                                                  static_cast<f32>(node->local_transform.rotation.y),
                                                  static_cast<f32>(node->local_transform.rotation.z)));
            t.scale = to_glm(node->local_transform.scale);
            skeleton->rest_pose.push_back(t);
            skeleton->inverse_bind.emplace_back(1.0f);
        }

        // ---- meshes ----------------------------------------------------------------------------
        struct MeshInfo {
            Asset model{};
            bool skinned = false;
            std::string node_name;
            std::vector<const ufbx_blend_channel *> channels; // blend channels, in morph-target order
        };
        std::vector<MeshInfo> mesh_infos(scene->meshes.count);
        std::vector<std::shared_ptr<Animation::Clip>> clips;

        const auto rollback = [&assets, &out]() {
            for (Asset model : out.models) (void)assets.unload(model);
            out.models.clear();
        };

        std::map<std::pair<const ufbx_texture *, int>, Asset> texture_cache;
        std::optional<Asset> flat_normal;
        const auto load_texture = [&](const ufbx_texture *texture, TextureColorSpace space, TextureKind kind) -> std::optional<Asset> {
            if (texture == nullptr) return std::nullopt;
            const auto key = std::make_pair(texture, static_cast<int>(kind) * 2 + static_cast<int>(space));
            if (const auto it = texture_cache.find(key); it != texture_cache.end()) return it->second;
            AssetExpected<Asset> loaded = std::unexpected(fbx_error(AssetErrorCode::IoFailure, "texture not found", source));
            if (texture->content.size > 0) {
                loaded = assets.create_texture_from_encoded_bytes(
                    std::span<const std::byte>{static_cast<const std::byte *>(texture->content.data), texture->content.size},
                    space, kind, UString{to_std(texture->name)});
            } else {
                const std::string names[] = {to_std(texture->filename), to_std(texture->relative_filename),
                                             to_std(texture->absolute_filename)};
                for (const std::string &name : names) {
                    if (name.empty()) continue;
                    const std::filesystem::path as_given{name};
                    const std::filesystem::path candidates[] = {as_given, base_dir / as_given,
                                                                base_dir / as_given.filename()};
                    for (const auto &candidate : candidates) {
                        std::error_code ec;
                        if (std::filesystem::is_regular_file(candidate, ec)) {
                            loaded = assets.load_texture(candidate, space, kind, UString{to_std(texture->name)});
                            break;
                        }
                    }
                    if (loaded) break;
                }
            }
            if (!loaded) return std::nullopt; // a missing texture degrades to the material's colour factor
            texture_cache.emplace(key, *loaded);
            return *loaded;
        };

        const auto flat_normal_texture = [&]() -> std::optional<Asset> {
            if (flat_normal) return flat_normal;
            AssetExpected<Asset> created = assets.create_texture(TextureAssetDesc{
                .width = 1, .height = 1, .color_space = TextureColorSpace::Linear,
                .pixels = std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}},
                .label = UString{"fbx flat normal"_ustr}});
            if (created) flat_normal = *created;
            return flat_normal;
        };

        if (!animations_only) {
            for (const ufbx_node *node : ordered) {
                if (node->mesh != nullptr && mesh_infos[node->mesh->typed_id].node_name.empty()) {
                    mesh_infos[node->mesh->typed_id].node_name = to_std(node->name);
                }
            }
            for (usize mi = 0; mi < scene->meshes.count; ++mi) {
                const ufbx_mesh *mesh = scene->meshes.data[mi];
                if (mesh->instances.count == 0 || mesh->num_triangles == 0) continue;
                const ufbx_node *instance_node = mesh->instances.data[0];
                const ufbx_skin_deformer *skin = mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;
                const ufbx_blend_deformer *blend = mesh->blend_deformers.count > 0 ? mesh->blend_deformers.data[0] : nullptr;
                const bool skinned = skin != nullptr && skin->clusters.count > 0;
                mesh_infos[mi].skinned = skinned;

                // Skinned vertices are authored in world space at bind time; rigid meshes keep their local space and
                // take the node transform as instance placement.
                ufbx_matrix to_world = instance_node->geometry_to_world;
                if (skinned && skin->clusters.data[0] != nullptr) to_world = skin->clusters.data[0]->geometry_to_world;
                const bool transform_vertices = skinned;

                if (skinned) {
                    for (usize ci = 0; ci < skin->clusters.count; ++ci) {
                        const ufbx_skin_cluster *cluster = skin->clusters.data[ci];
                        if (cluster->bone_node == nullptr) continue;
                        const ufbx_matrix inverse_bind = ufbx_matrix_invert(&cluster->bind_to_world);
                        skeleton->inverse_bind[node_to_joint[cluster->bone_node->typed_id]] = to_glm(inverse_bind);
                    }
                }
                if (blend != nullptr) {
                    for (usize ci = 0; ci < blend->channels.count; ++ci) mesh_infos[mi].channels.push_back(blend->channels.data[ci]);
                }

                ModelAssetDesc desc{.label = UString{to_std(mesh->name.length > 0 ? mesh->name : instance_node->name)}};
                if (desc.label.empty()) desc.label = UString{"fbx_mesh"_ustr};
                std::vector<Detail::ImportMaterialValues> pending;

                // One primitive per material part (or the whole mesh when it has no material assignment).
                std::vector<std::vector<u32>> part_faces;
                if (mesh->material_parts.count > 0) {
                    for (usize pi = 0; pi < mesh->material_parts.count; ++pi) {
                        const ufbx_mesh_part &part = mesh->material_parts.data[pi];
                        part_faces.emplace_back(part.face_indices.data, part.face_indices.data + part.face_indices.count);
                    }
                } else {
                    std::vector<u32> all(mesh->num_faces);
                    for (usize f = 0; f < all.size(); ++f) all[f] = static_cast<u32>(f);
                    part_faces.push_back(std::move(all));
                }

                std::vector<u32> triangle_scratch(std::max<usize>(mesh->max_face_triangles, 1) * 3);
                for (usize pi = 0; pi < part_faces.size(); ++pi) {
                    std::vector<Renderer::GeometryVertex> vertices;
                    std::vector<u32> indices;
                    std::vector<u32> source_vertex;
                    std::unordered_map<VertexKey, u32, VertexKeyHash> weld;
                    bool has_tangents = false;

                    for (u32 face_index : part_faces[pi]) {
                        const ufbx_face face = mesh->faces.data[face_index];
                        if (face.num_indices < 3) continue;
                        const u32 triangles = ufbx_triangulate_face(triangle_scratch.data(), triangle_scratch.size(), mesh, face);
                        for (u32 corner = 0; corner < triangles * 3; ++corner) {
                            const u32 ix = triangle_scratch[corner];
                            const VertexKey key{
                                mesh->vertex_indices.data[ix],
                                mesh->vertex_normal.exists ? mesh->vertex_normal.indices.data[ix] : 0u,
                                mesh->vertex_uv.exists ? mesh->vertex_uv.indices.data[ix] : 0u,
                                mesh->vertex_color.exists ? mesh->vertex_color.indices.data[ix] : 0u};
                            const auto [it, inserted] = weld.try_emplace(key, static_cast<u32>(vertices.size()));
                            if (inserted) {
                                Renderer::GeometryVertex v;
                                glm::vec3 position = to_glm(ufbx_get_vertex_vec3(&mesh->vertex_position, ix));
                                glm::vec3 normal = mesh->vertex_normal.exists ? to_glm(ufbx_get_vertex_vec3(&mesh->vertex_normal, ix))
                                                                              : glm::vec3(0, 1, 0);
                                if (transform_vertices) {
                                    position = to_glm(ufbx_transform_position(&to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, ix)));
                                    normal = glm::normalize(to_glm(ufbx_transform_direction(&to_world, ufbx_get_vertex_vec3(&mesh->vertex_normal, ix))));
                                }
                                v.position = position;
                                v.normal = normal;
                                if (mesh->vertex_uv.exists) {
                                    const glm::vec2 uv = to_glm(ufbx_get_vertex_vec2(&mesh->vertex_uv, ix));
                                    v.uv = {uv.x, 1.0f - uv.y}; // FBX V runs up, the engine's runs down
                                }
                                if (mesh->vertex_color.exists) v.color = to_glm(ufbx_get_vertex_vec4(&mesh->vertex_color, ix));
                                if (mesh->vertex_tangent.exists) {
                                    glm::vec3 tangent = to_glm(ufbx_get_vertex_vec3(&mesh->vertex_tangent, ix));
                                    if (transform_vertices) tangent = to_glm(ufbx_transform_direction(&to_world, ufbx_get_vertex_vec3(&mesh->vertex_tangent, ix)));
                                    tangent = glm::normalize(tangent);
                                    f32 handedness = 1.0f;
                                    if (mesh->vertex_bitangent.exists) {
                                        glm::vec3 bitangent = to_glm(ufbx_get_vertex_vec3(&mesh->vertex_bitangent, ix));
                                        handedness = glm::dot(glm::cross(v.normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                                    }
                                    v.tangent = glm::vec4(tangent, handedness);
                                    has_tangents = true;
                                }
                                vertices.push_back(v);
                                source_vertex.push_back(mesh->vertex_indices.data[ix]);
                            }
                            indices.push_back(it->second);
                        }
                    }
                    if (vertices.empty()) continue;
                    if (!has_tangents && mesh->vertex_uv.exists) Detail::generate_tangents(vertices, indices);

                    ModelPrimitiveDesc primitive{
                        .mesh = Renderer::Mesh::from_vertices(vertices, indices, (desc.label.cpp_string() + " part " + std::to_string(pi)).c_str()),
                        .shader = shader,
                    };

                    if (skinned) {
                        auto weights = std::make_shared<Animation::SkinWeights>();
                        weights->joints.assign(vertices.size(), glm::uvec4{0});
                        weights->weights.assign(vertices.size(), glm::vec4{0.0f});
                        for (usize v = 0; v < vertices.size(); ++v) {
                            const ufbx_skin_vertex sv = skin->vertices.data[source_vertex[v]];
                            std::vector<std::pair<f32, u32>> influences;
                            for (u32 w = 0; w < sv.num_weights; ++w) {
                                const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + w];
                                const ufbx_node *bone = skin->clusters.data[sw.cluster_index]->bone_node;
                                if (bone != nullptr && sw.weight > 0.0) {
                                    influences.emplace_back(static_cast<f32>(sw.weight), node_to_joint[bone->typed_id]);
                                }
                            }
                            std::sort(influences.begin(), influences.end(), [](auto &a, auto &b) { return a.first > b.first; });
                            if (influences.size() > 4) influences.resize(4);
                            f32 total = 0.0f;
                            for (auto &[w, j] : influences) total += w;
                            for (usize k = 0; k < influences.size(); ++k) {
                                weights->joints[v][static_cast<int>(k)] = influences[k].second;
                                weights->weights[v][static_cast<int>(k)] = total > 0.0f ? influences[k].first / total : 0.0f;
                            }
                        }
                        primitive.skin = std::move(weights);
                    }

                    if (!mesh_infos[mi].channels.empty()) {
                        Animation::MorphBuilder morph(vertices.size());
                        for (const ufbx_blend_channel *channel : mesh_infos[mi].channels) {
                            const ufbx_blend_shape *shape = channel->target_shape;
                            if (shape == nullptr && channel->keyframes.count > 0) shape = channel->keyframes.data[channel->keyframes.count - 1].shape;
                            std::vector<glm::vec3> dense_position(mesh->num_vertices, glm::vec3{0.0f});
                            std::vector<glm::vec3> dense_normal;
                            if (shape != nullptr) {
                                if (shape->normal_offsets.count > 0) dense_normal.assign(mesh->num_vertices, glm::vec3{0.0f});
                                for (usize k = 0; k < shape->num_offsets; ++k) {
                                    const u32 sv = shape->offset_vertices.data[k];
                                    if (sv >= mesh->num_vertices) continue;
                                    dense_position[sv] = to_glm(shape->position_offsets.data[k]);
                                    if (!dense_normal.empty() && k < shape->normal_offsets.count) dense_normal[sv] = to_glm(shape->normal_offsets.data[k]);
                                }
                            }
                            std::vector<glm::vec3> out_position(vertices.size()), out_normal;
                            if (!dense_normal.empty()) out_normal.resize(vertices.size());
                            for (usize v = 0; v < vertices.size(); ++v) {
                                glm::vec3 dp = dense_position[source_vertex[v]];
                                if (transform_vertices) dp = to_glm(ufbx_transform_direction(&to_world, ufbx_vec3{dp.x, dp.y, dp.z}));
                                out_position[v] = dp;
                                if (!out_normal.empty()) out_normal[v] = dense_normal[source_vertex[v]];
                            }
                            morph.add_target(to_std(channel->name), out_position, out_normal, static_cast<f32>(channel->weight));
                        }
                        primitive.morph = std::make_shared<const Animation::MorphTargetSet>(morph.build());
                    }

                    Detail::ImportMaterialValues values;
                    const ufbx_material *material = nullptr;
                    if (mesh->material_parts.count > 0 && mesh->material_parts.data[pi].index < mesh->materials.count) {
                        material = mesh->materials.data[mesh->material_parts.data[pi].index];
                    } else if (mesh->materials.count > 0) {
                        material = mesh->materials.data[0];
                    }
                    values.metallic_factor = 0.0f;
                    values.roughness_factor = 0.6f;
                    std::optional<Asset> normal_texture;
                    if (material != nullptr) {
                        const ufbx_material_pbr_maps &pbr = material->pbr;
                        if (pbr.base_color.has_value) {
                            const f32 factor = pbr.base_factor.has_value ? static_cast<f32>(pbr.base_factor.value_real) : 1.0f;
                            values.base_color_factor = glm::vec4(to_glm(pbr.base_color.value_vec3) * factor, 1.0f);
                        }
                        if (pbr.opacity.has_value) values.base_color_factor.a = static_cast<f32>(pbr.opacity.value_real);
                        if (pbr.metalness.has_value) values.metallic_factor = static_cast<f32>(pbr.metalness.value_real);
                        if (pbr.roughness.has_value) values.roughness_factor = static_cast<f32>(pbr.roughness.value_real);
                        else if (pbr.glossiness.has_value) values.roughness_factor = 1.0f - static_cast<f32>(pbr.glossiness.value_real);
                        if (pbr.emission_color.has_value) {
                            const f32 factor = pbr.emission_factor.has_value ? static_cast<f32>(pbr.emission_factor.value_real) : 1.0f;
                            values.emissive_factor = glm::vec4(to_glm(pbr.emission_color.value_vec3) * factor, 0.0f);
                        }
                        if (const auto texture = load_texture(pbr.base_color.texture, TextureColorSpace::Srgb,
                                                              values.base_color_factor.a < 1.0f ? TextureKind::ColorAlpha : TextureKind::ColorOpaque)) {
                            primitive.textures.push_back(ModelTextureBinding{.slot = UString{"base_color_texture"_ustr}, .texture = *texture});
                            values.base_color_factor = glm::vec4(1.0f, 1.0f, 1.0f, values.base_color_factor.a);
                        }
                        normal_texture = load_texture(pbr.normal_map.texture, TextureColorSpace::Linear, TextureKind::NormalMap);
                        if (const auto texture = load_texture(pbr.emission_color.texture, TextureColorSpace::Srgb, TextureKind::ColorAlpha)) {
                            primitive.textures.push_back(ModelTextureBinding{.slot = UString{"emissive_texture"_ustr}, .texture = *texture});
                            values.emissive_factor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
                        }
                    }
                    if (!normal_texture) normal_texture = flat_normal_texture();
                    if (normal_texture) {
                        primitive.textures.push_back(ModelTextureBinding{.slot = UString{"normal_texture"_ustr}, .texture = *normal_texture});
                    }
                    desc.primitives.push_back(std::move(primitive));
                    pending.push_back(values);
                }
                if (desc.primitives.empty()) continue;

                AssetExpected<Asset> model = assets.create_model(std::move(desc));
                if (!model) {
                    rollback();
                    return std::unexpected(model.error());
                }
                for (usize p = 0; p < pending.size(); ++p) {
                    if (AssetResult set = Detail::apply_material_values(assets, *model, p, pending[p]); !set) {
                        (void)assets.unload(*model);
                        rollback();
                        return std::unexpected(set.error());
                    }
                }
                mesh_infos[mi].model = *model;
                out.models.push_back(*model);
            }
        }

        // ---- animation stacks, baked to linear keys ---------------------------------------------
        for (usize si = 0; si < scene->anim_stacks.count; ++si) {
            const ufbx_anim_stack *stack = scene->anim_stacks.data[si];
            ufbx_bake_opts bake_opts{};
            bake_opts.resample_rate = 30.0;
            bake_opts.max_keyframe_segments = 32;
            ufbx_error bake_error{};
            std::unique_ptr<ufbx_baked_anim, BakeDeleter> bake{ufbx_bake_anim(scene.get(), stack->anim, &bake_opts, &bake_error)};
            if (!bake) continue;

            auto clip = std::make_shared<Animation::Clip>();
            clip->name = UString{stack->name.length > 0 ? to_std(stack->name) : std::string{"animation"}};
            clip->channels.resize(skeleton->joint_count());
            clip->joint_names = skeleton->names;
            const double origin = stack->time_begin;
            bool any = false;
            for (usize i = 0; i < bake->nodes.count; ++i) {
                const ufbx_baked_node &bn = bake->nodes.data[i];
                if (bn.typed_id >= node_count) continue;
                Animation::JointChannels &channels = clip->channels[node_to_joint[bn.typed_id]];
                append_vec3_keys(channels.translation, bn.translation_keys, origin, bn.constant_translation);
                append_quat_keys(channels.rotation, bn.rotation_keys, origin, bn.constant_rotation);
                append_vec3_keys(channels.scale, bn.scale_keys, origin, bn.constant_scale);
                any = true;
            }

            // Blend-shape weights ride in baked element props ("DeformPercent", 0..100) keyed by channel element.
            std::unordered_map<u32, const ufbx_baked_prop *> weight_props;
            for (usize i = 0; i < bake->elements.count; ++i) {
                const ufbx_baked_element &element = bake->elements.data[i];
                for (usize p = 0; p < element.props.count; ++p) {
                    if (to_std(element.props.data[p].name) == UFBX_DeformPercent) weight_props[element.element_id] = &element.props.data[p];
                }
            }
            for (const MeshInfo &info : mesh_infos) {
                if (info.channels.empty() || info.node_name.empty()) continue;
                std::vector<f32> times;
                bool animated = false;
                for (const ufbx_blend_channel *channel : info.channels) {
                    const auto it = weight_props.find(channel->element_id);
                    if (it == weight_props.end() || it->second->keys.count == 0) continue;
                    animated = true;
                    for (usize k = 0; k < it->second->keys.count; ++k) {
                        times.push_back(static_cast<f32>(std::max(0.0, it->second->keys.data[k].time - origin)));
                    }
                }
                if (!animated) continue;
                std::sort(times.begin(), times.end());
                times.erase(std::unique(times.begin(), times.end(), [](f32 a, f32 b) { return std::fabs(a - b) < 1e-6f; }), times.end());
                Animation::MorphTrack track;
                track.target = UString{info.node_name};
                track.weight_count = static_cast<u32>(info.channels.size());
                track.track.interpolation = Animation::Interpolation::Linear;
                track.track.times = times;
                for (f32 time : times) {
                    for (const ufbx_blend_channel *channel : info.channels) {
                        f32 weight = static_cast<f32>(channel->weight);
                        if (const auto it = weight_props.find(channel->element_id); it != weight_props.end() && it->second->keys.count > 0) {
                            const auto &keys = it->second->keys;
                            const double t = time + origin;
                            if (t <= keys.data[0].time) weight = static_cast<f32>(keys.data[0].value.x) / 100.0f;
                            else if (t >= keys.data[keys.count - 1].time) weight = static_cast<f32>(keys.data[keys.count - 1].value.x) / 100.0f;
                            else {
                                for (usize k = 0; k + 1 < keys.count; ++k) {
                                    if (t >= keys.data[k].time && t <= keys.data[k + 1].time) {
                                        const double span = keys.data[k + 1].time - keys.data[k].time;
                                        const double f = span > 0.0 ? (t - keys.data[k].time) / span : 0.0;
                                        weight = static_cast<f32>((keys.data[k].value.x + (keys.data[k + 1].value.x - keys.data[k].value.x) * f) / 100.0);
                                        break;
                                    }
                                }
                            }
                        }
                        track.track.values.push_back(weight);
                    }
                }
                clip->morph_tracks.push_back(std::move(track));
                any = true;
            }
            if (!any) continue;
            clip->duration = static_cast<f32>(std::max(0.0, stack->time_end - stack->time_begin));
            clip->recompute_duration();
            clips.push_back(std::move(clip));
        }

        // ---- the scene description ---------------------------------------------------------------
        out.scene_skeleton = skeleton;
        for (auto &clip : clips) out.scene_clips.push_back(clip);

        const bool any_skinned = std::any_of(mesh_infos.begin(), mesh_infos.end(), [](const MeshInfo &m) { return m.skinned; });
        if (any_skinned) {
            out.skins.push_back(GltfSkin{.name = UString{"fbx_skin"_ustr}, .skeleton = skeleton, .clips = out.scene_clips});
        }
        for (const ufbx_node *node : ordered) {
            if (node->mesh == nullptr || node->mesh->typed_id >= mesh_infos.size() || !mesh_infos[node->mesh->typed_id].model) continue;
            const MeshInfo &info = mesh_infos[node->mesh->typed_id];
            out.instances.push_back(GltfNodeInstance{
                .name = UString{to_std(node->name)},
                .model = info.model,
                .world_transform = info.skinned ? glm::mat4{1.0f} : to_glm(node->geometry_to_world),
                .skin = info.skinned ? 0 : -1,
                .scene_joint = static_cast<i32>(node_to_joint[node->typed_id]),
            });
        }
        Foundation::log_info("FbxImport: '{}': {} model(s), {} instance(s), {} joint(s), {} clip(s){}", source.string(),
                             out.models.size(), out.instances.size(), skeleton->joint_count(), out.scene_clips.size(),
                             any_skinned ? ", skinned" : "");
        return out;
    }

} // namespace SFT::Engine
