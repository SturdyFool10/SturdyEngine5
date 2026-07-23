#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        constexpr u32 kAtlasGridSize = 8;
        constexpr f32 kMinimumLightRange = 0.05f;

        [[nodiscard]] Core::GraphicsBackendError shadow_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        [[nodiscard]] glm::vec3 safe_normalize(glm::vec3 value, glm::vec3 fallback) noexcept {
            const f32 length_squared = glm::dot(value, value);
            return std::isfinite(length_squared) && length_squared > 1.0e-12f
                       ? value * glm::inversesqrt(length_squared)
                       : fallback;
        }

        [[nodiscard]] f32 finite_or(f32 value, f32 fallback) noexcept {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] glm::vec3 light_up(glm::vec3 direction) noexcept {
            return std::abs(direction.y) < 0.98f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                                 : glm::vec3{0.0f, 0.0f, 1.0f};
        }

        [[nodiscard]] f32 luminance(glm::vec3 radiance) noexcept {
            return glm::dot(glm::max(radiance, glm::vec3{0.0f}), glm::vec3{0.2126f, 0.7152f, 0.0722f});
        }

        [[nodiscard]] f32 punctual_importance(glm::vec3 position, glm::vec3 radiance, f32 range, glm::vec3 camera_position) noexcept {
            const glm::vec3 offset = position - camera_position;
            const f32 distance_squared = glm::dot(offset, offset);
            const f32 safe_range = std::max(range, kMinimumLightRange);
            return luminance(radiance) * safe_range * safe_range /
                   (1.0f + distance_squared / (safe_range * safe_range));
        }

        struct AtlasTile {
            u32 x = 0;
            u32 y = 0;
            u32 cells = 0;
            explicit operator bool() const noexcept { return cells != 0; }
        };

        class AtlasAllocator {
          public:
            [[nodiscard]] AtlasTile allocate(u32 cells) noexcept {
                if (cells == 0 || cells > kAtlasGridSize) {
                    return {};
                }
                for (u32 y = 0; y + cells <= kAtlasGridSize; ++y) {
                    for (u32 x = 0; x + cells <= kAtlasGridSize; ++x) {
                        bool free = true;
                        for (u32 row = 0; row < cells && free; ++row) {
                            for (u32 column = 0; column < cells; ++column) {
                                free &= !used_[(y + row) * kAtlasGridSize + x + column];
                            }
                        }
                        if (!free) {
                            continue;
                        }
                        for (u32 row = 0; row < cells; ++row) {
                            for (u32 column = 0; column < cells; ++column) {
                                used_[(y + row) * kAtlasGridSize + x + column] = true;
                            }
                        }
                        return AtlasTile{.x = x, .y = y, .cells = cells};
                    }
                }
                return {};
            }

          private:
            array<bool, kAtlasGridSize * kAtlasGridSize> used_{};
        };

        [[nodiscard]] RHI::Rect2D tile_viewport(AtlasTile tile, u32 atlas_size) noexcept {
            const u32 cell_size = atlas_size / kAtlasGridSize;
            return RHI::Rect2D{
                .x = static_cast<i32>(tile.x * cell_size),
                .y = static_cast<i32>(tile.y * cell_size),
                .width = tile.cells * cell_size,
                .height = tile.cells * cell_size,
            };
        }

        [[nodiscard]] glm::vec4 tile_scale_bias(AtlasTile tile) noexcept {
            const f32 inv_grid = 1.0f / static_cast<f32>(kAtlasGridSize);
            return glm::vec4{
                static_cast<f32>(tile.cells) * inv_grid,
                static_cast<f32>(tile.cells) * inv_grid,
                static_cast<f32>(tile.x) * inv_grid,
                static_cast<f32>(tile.y) * inv_grid,
            };
        }

        [[nodiscard]] glm::mat4 stabilize_directional_projection(glm::mat4 projection,
                                                                 const glm::mat4 &view,
                                                                 u32 resolution) noexcept {
            // Michal Valient's stable-CSM snap: anchor the light projection to integer shadow texels
            // so camera translation cannot make an otherwise-static edge swim through the atlas.
            glm::vec4 origin = projection * view * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
            origin *= static_cast<f32>(resolution) * 0.5f;
            const glm::vec4 rounded = glm::round(origin);
            glm::vec4 offset = (rounded - origin) * (2.0f / static_cast<f32>(resolution));
            offset.z = 0.0f;
            offset.w = 0.0f;
            projection[3] += offset;
            return projection;
        }

        [[nodiscard]] usize bind_group_layout_index_for_set(span<const u32> sets, u32 set) noexcept {
            for (usize i = 0; i < sets.size(); ++i) {
                if (sets[i] == set) {
                    return i;
                }
            }
            return sets.size();
        }
    } // namespace

    Core::RendererResult Renderer::ensure_frame_shadow_targets(FrameInFlight &slot, u32 requested_atlas_size) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot allocate shadow targets without an RHI device."));
        }

        const u32 device_max = std::max(device->limits().max_texture_dimension_2d, 512u);
        u32 atlas_size = requested_atlas_size == 0
                             ? 0u
                             : std::clamp(requested_atlas_size, 512u, std::min(16384u, device_max));
        atlas_size -= atlas_size % kAtlasGridSize;
        if (!slot.shadow_targets.lighting_buffer) {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = sizeof(ShadowLightingGpuData),
                .usage = RHI::BufferUsage::Uniform,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "shadow lighting constants",
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), "create shadow lighting constants buffer"));
            }
            slot.shadow_targets.lighting_buffer = *buffer;
        }

        if (atlas_size == 0) {
            if (slot.shadow_targets.atlas_view) {
                device->destroy_texture_view(slot.shadow_targets.atlas_view);
            }
            if (slot.shadow_targets.atlas) {
                device->destroy_texture(slot.shadow_targets.atlas);
            }
            slot.shadow_targets.atlas = {};
            slot.shadow_targets.atlas_view = {};
            slot.shadow_targets.atlas_size = 0;
            return {};
        }
        if (slot.shadow_targets.atlas && slot.shadow_targets.atlas_size == atlas_size) {
            return {};
        }

        if (slot.shadow_targets.atlas_view) {
            device->destroy_texture_view(slot.shadow_targets.atlas_view);
        }
        if (slot.shadow_targets.atlas) {
            device->destroy_texture(slot.shadow_targets.atlas);
        }
        slot.shadow_targets.atlas = {};
        slot.shadow_targets.atlas_view = {};

        auto atlas = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = slot.shadow_targets.format,
            .extent = RHI::Extent3D{.width = atlas_size, .height = atlas_size, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
            .label = "raster shadow atlas",
        });
        if (!atlas) {
            return unexpected(graphics_error_from_rhi(atlas.error(), "create raster shadow atlas"));
        }
        auto atlas_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *atlas,
            .view_type = RHI::TextureViewType::View2D,
            .label = "raster shadow atlas view",
        });
        if (!atlas_view) {
            device->destroy_texture(*atlas);
            return unexpected(graphics_error_from_rhi(atlas_view.error(), "create raster shadow atlas view"));
        }
        slot.shadow_targets.atlas = *atlas;
        slot.shadow_targets.atlas_view = *atlas_view;
        slot.shadow_targets.atlas_size = atlas_size;
        return {};
    }

    void Renderer::destroy_frame_shadow_targets(FrameInFlight &slot) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            if (slot.shadow_targets.atlas_view) {
                device->destroy_texture_view(slot.shadow_targets.atlas_view);
            }
            if (slot.shadow_targets.atlas) {
                device->destroy_texture(slot.shadow_targets.atlas);
            }
            if (slot.shadow_targets.lighting_buffer) {
                device->destroy_buffer(slot.shadow_targets.lighting_buffer);
            }
        }
        slot.shadow_targets = {};
    }

    Core::RendererResult Renderer::prepare_shadow_frame(const FrameSubmission &submission,
                                                        FrameShadowTargets &targets,
                                                        PreparedShadowFrame &prepared,
                                                        Core::Extent2D render_extent) {
        static_assert(sizeof(ShadowViewGpuData) == 112);
        static_assert(sizeof(DirectionalLightGpuData) == 64);
        static_assert(sizeof(SpotLightGpuData) == 64);
        static_assert(sizeof(PointLightGpuData) == 48);
        static_assert(sizeof(ShadowLightingGpuData) == 5232);
        static_assert(offsetof(ShadowLightingGpuData, sun) == 240);
        static_assert(offsetof(ShadowLightingGpuData, spot_lights) == 304);
        static_assert(offsetof(ShadowLightingGpuData, point_lights) == 816);
        static_assert(offsetof(ShadowLightingGpuData, shadow_views) == 1200);
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !targets.lighting_buffer) {
            return unexpected(shadow_error("Cannot prepare shadow lighting without its per-frame constant buffer."));
        }

        prepared = {};
        ShadowLightingGpuData &gpu = prepared.gpu;
        const glm::mat4 view_projection = submission.camera.projection * submission.camera.view;
        gpu.inverse_view_projection = glm::inverse(view_projection);
        gpu.view = submission.camera.view;
        gpu.camera_position_near = glm::vec4{submission.camera.world_position,
                                             std::max(submission.camera.near_plane, 0.0001f)};
        gpu.ambient_radiance_exposure = glm::vec4{
            glm::max(submission.lighting.ambient_radiance, glm::vec3{0.0f}),
            std::max(submission.lighting.exposure, 0.0f),
        };
        gpu.background_color = submission.render_graph.background_color *
                               glm::vec4{submission.render_graph.background_intensity,
                                         submission.render_graph.background_intensity,
                                         submission.render_graph.background_intensity,
                                         1.0f};
        gpu.gtao_params = glm::vec4{
            std::max(finite_or(submission.render_graph.gtao_radius, 1.0f), 0.001f),
            std::clamp(finite_or(submission.render_graph.gtao_falloff, 0.8f), 0.0f, 0.999f),
            std::max(finite_or(submission.render_graph.gtao_thickness, 0.15f), 0.0f),
            std::clamp(finite_or(submission.render_graph.gtao_intensity, 1.0f), 0.0f, 4.0f),
        };
        gpu.viewport_params = glm::vec4{
            1.0f / static_cast<f32>(std::max(render_extent.width, 1u)),
            1.0f / static_cast<f32>(std::max(render_extent.height, 1u)),
            std::abs(submission.camera.projection[1][1]),
            submission.render_graph.ambient_occlusion
                ? static_cast<f32>(std::min(submission.render_graph.gtao_quality, 3u) + 1u)
                : 0.0f,
        };

        const DirectionalLight &sun = submission.lighting.sun;
        const glm::vec3 sun_direction = safe_normalize(sun.direction, glm::vec3{0.0f, -1.0f, 0.0f});
        gpu.sun.direction_angular_radius = glm::vec4{
            sun_direction,
            glm::radians(std::clamp(finite_or(sun.angular_radius_degrees, 0.27f), 0.0f, 10.0f)),
        };
        gpu.sun.radiance_shadow = glm::vec4{glm::max(sun.radiance, glm::vec3{0.0f}), 0.0f};

        const bool shadows_enabled = submission.render_graph.shadows && static_cast<bool>(targets.atlas);
        const u32 atlas_size = targets.atlas_size;
        gpu.shadow_params = glm::vec4{
            atlas_size > 0 ? 1.0f / static_cast<f32>(atlas_size) : 1.0f,
            std::clamp(finite_or(submission.render_graph.shadow_normal_bias, 0.75f), 0.0f, 4.0f),
            submission.render_graph.shadow_contact_hardening ? 1.0f : 0.0f,
            std::max(finite_or(submission.render_graph.shadow_max_distance, 250.0f), submission.camera.near_plane),
        };

        AtlasAllocator allocator;
        auto append_shadow_view = [&](const glm::mat4 &matrix, AtlasTile tile, f32 near_plane,
                                      f32 far_plane, bool perspective, f32 light_radius_uv,
                                      f32 world_span_at_unit_depth) -> i32 {
            if (!tile || prepared.render_views.size() >= max_shadow_views) {
                return -1;
            }
            const RHI::Rect2D viewport = tile_viewport(tile, atlas_size);
            const usize index = prepared.render_views.size();
            prepared.render_views.push_back(ShadowRenderView{
                .view_projection = matrix,
                .frustum = frustum_from_view_projection(matrix),
                .viewport = viewport,
            });
            gpu.shadow_views[index] = ShadowViewGpuData{
                .view_projection = matrix,
                .atlas_scale_bias = tile_scale_bias(tile),
                .depth_params = glm::vec4{near_plane, far_plane, perspective ? 1.0f : 0.0f, std::max(light_radius_uv, 0.0f)},
                .filter_params = glm::vec4{world_span_at_unit_depth,
                                           static_cast<f32>(viewport.width), 0.0f, 0.0f},
            };
            return static_cast<i32>(index);
        };

        // Directional CSM: practical logarithmic/uniform split blend, bounding-sphere fits, radius
        // quantization, and projection texel snapping. The two detail cascades receive 3x3 cells;
        // the distant pair receive 2x2, leaving enough atlas space for every configured local map.
        if (shadows_enabled && sun.casts_shadows && luminance(sun.radiance) > 0.0f) {
            const u32 cascade_count = std::clamp(submission.render_graph.shadow_cascade_count,
                                                 1u,
                                                 max_directional_shadow_cascades);
            const f32 camera_near = std::max(submission.camera.near_plane, 0.0001f);
            const f32 camera_far = std::max(camera_near + 0.01f,
                                            std::min(submission.camera.far_plane,
                                                     finite_or(submission.render_graph.shadow_max_distance, 250.0f)));
            const f32 split_lambda = std::clamp(
                finite_or(submission.render_graph.shadow_cascade_split_lambda, 0.65f),
                0.0f,
                1.0f);
            array<f32, max_directional_shadow_cascades> splits{};
            for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
                const f32 p = static_cast<f32>(cascade + 1) / static_cast<f32>(cascade_count);
                const f32 logarithmic = camera_near * std::pow(camera_far / camera_near, p);
                const f32 uniform = camera_near + (camera_far - camera_near) * p;
                splits[cascade] = glm::mix(uniform, logarithmic, split_lambda);
                gpu.sun.cascade_splits[cascade] = splits[cascade];
            }

            array<glm::vec3, 4> camera_near_corners{};
            array<glm::vec3, 4> camera_far_corners{};
            const array<glm::vec2, 4> ndc_xy{
                glm::vec2{-1.0f, -1.0f},
                glm::vec2{1.0f, -1.0f},
                glm::vec2{1.0f, 1.0f},
                glm::vec2{-1.0f, 1.0f},
            };
            for (usize corner = 0; corner < ndc_xy.size(); ++corner) {
                glm::vec4 a = gpu.inverse_view_projection * glm::vec4{ndc_xy[corner], 0.0f, 1.0f};
                glm::vec4 b = gpu.inverse_view_projection * glm::vec4{ndc_xy[corner], 1.0f, 1.0f};
                a /= a.w;
                b /= b.w;
                const f32 a_depth = std::abs((submission.camera.view * a).z);
                const f32 b_depth = std::abs((submission.camera.view * b).z);
                camera_near_corners[corner] = glm::vec3{a_depth <= b_depth ? a : b};
                camera_far_corners[corner] = glm::vec3{a_depth <= b_depth ? b : a};
            }

            const f32 inverse_camera_span = 1.0f / std::max(submission.camera.far_plane - submission.camera.near_plane,
                                                            0.0001f);
            f32 previous_split = camera_near;
            i32 first_cascade_view = -1;
            u32 emitted_cascades = 0;
            for (u32 cascade = 0; cascade < cascade_count; ++cascade) {
                const AtlasTile tile = allocator.allocate(cascade < 2 ? 3u : 2u);
                if (!tile) {
                    break;
                }
                const f32 near_t = glm::clamp((previous_split - submission.camera.near_plane) * inverse_camera_span,
                                              0.0f,
                                              1.0f);
                const f32 far_t = glm::clamp((splits[cascade] - submission.camera.near_plane) * inverse_camera_span,
                                             0.0f,
                                             1.0f);
                array<glm::vec3, 8> corners{};
                glm::vec3 center{0.0f};
                for (usize corner = 0; corner < 4; ++corner) {
                    corners[corner] = glm::mix(camera_near_corners[corner], camera_far_corners[corner], near_t);
                    corners[corner + 4] = glm::mix(camera_near_corners[corner], camera_far_corners[corner], far_t);
                    center += corners[corner] + corners[corner + 4];
                }
                center *= 1.0f / 8.0f;
                f32 radius = 0.0f;
                for (glm::vec3 corner : corners) {
                    radius = std::max(radius, glm::distance(center, corner));
                }
                radius = std::ceil(std::max(radius, 0.25f) * 16.0f) / 16.0f;

                // Fit the light-space depth range to all casters that overlap this cascade in XY.
                // A fixed padding misses tall/off-camera casters in some sun directions and wastes
                // most of D32's precision in others. Bounding spheres keep the test conservative.
                const glm::mat4 orientation_view =
                    glm::lookAtRH(center - sun_direction, center, light_up(sun_direction));
                f32 minimum_along_light = -radius;
                f32 maximum_along_light = radius;
                for (const RenderItem &item : submission.draws) {
                    const MeshResource *mesh_resource = mesh(item.mesh);
                    if (mesh_resource == nullptr) {
                        continue;
                    }
                    const f32 scale_x = glm::length(glm::vec3{item.world_transform[0]});
                    const f32 scale_y = glm::length(glm::vec3{item.world_transform[1]});
                    const f32 scale_z = glm::length(glm::vec3{item.world_transform[2]});
                    const f32 maximum_scale = std::max({scale_x, scale_y, scale_z});
                    const f32 world_radius = mesh_resource->bounds_radius * maximum_scale;
                    const glm::vec3 world_center =
                        glm::vec3{item.world_transform * glm::vec4{mesh_resource->bounds_center, 1.0f}};
                    if (!std::isfinite(world_radius) ||
                        !std::isfinite(world_center.x) || !std::isfinite(world_center.y) ||
                        !std::isfinite(world_center.z)) {
                        continue;
                    }
                    const glm::vec3 oriented_center =
                        glm::vec3{orientation_view * glm::vec4{world_center, 1.0f}};
                    if (std::abs(oriented_center.x) > radius + world_radius ||
                        std::abs(oriented_center.y) > radius + world_radius) {
                        continue;
                    }
                    const f32 along_light = glm::dot(world_center - center, sun_direction);
                    minimum_along_light = std::min(minimum_along_light, along_light - world_radius);
                    maximum_along_light = std::max(maximum_along_light, along_light + world_radius);
                }

                const f32 cascade_resolution =
                    static_cast<f32>(tile_viewport(tile, atlas_size).width);
                const f32 world_texel = (2.0f * radius) / std::max(cascade_resolution, 1.0f);
                const f32 depth_margin = std::max(0.05f, world_texel * 4.0f);
                const f32 eye_distance = -minimum_along_light + depth_margin;
                const glm::vec3 eye = center - sun_direction * eye_distance;
                const glm::mat4 light_view = glm::lookAtRH(eye, center, light_up(sun_direction));
                const f32 shadow_near = std::max(
                    0.01f, eye_distance + minimum_along_light - depth_margin * 0.5f);
                const f32 shadow_far = std::max(
                    shadow_near + 0.01f, eye_distance + maximum_along_light + depth_margin);
                glm::mat4 light_projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, shadow_near, shadow_far);
                light_projection = stabilize_directional_projection(light_projection, light_view, tile_viewport(tile, atlas_size).width);
                const glm::mat4 light_view_projection = light_projection * light_view;
                const f32 local_radius_per_world = std::tan(gpu.sun.direction_angular_radius.w) /
                                                   std::max(2.0f * radius, 0.001f);
                const i32 view_index = append_shadow_view(light_view_projection, tile, shadow_near, shadow_far, false, local_radius_per_world, radius * 2.0f);
                if (view_index < 0) {
                    break;
                }
                if (first_cascade_view < 0) {
                    first_cascade_view = view_index;
                }
                ++emitted_cascades;
                previous_split = splits[cascade];
            }
            if (emitted_cascades > 0) {
                gpu.sun.radiance_shadow.w = 1.0f;
                gpu.sun.cascade_params = glm::vec4{
                    static_cast<f32>(emitted_cascades),
                    std::clamp(finite_or(submission.render_graph.shadow_cascade_blend, 0.10f), 0.0f, 0.5f),
                    static_cast<f32>(first_cascade_view),
                    0.0f,
                };
            }
        }

        vector<usize> spot_order(submission.lighting.spot_lights.size());
        std::iota(spot_order.begin(), spot_order.end(), usize{0});
        std::stable_sort(spot_order.begin(), spot_order.end(), [&](usize a, usize b) {
            const SpotLight &left = submission.lighting.spot_lights[a];
            const SpotLight &right = submission.lighting.spot_lights[b];
            return punctual_importance(left.position, left.radiance, left.range, submission.camera.world_position) >
                   punctual_importance(right.position, right.radiance, right.range, submission.camera.world_position);
        });
        const u32 spot_count = static_cast<u32>(std::min<usize>(spot_order.size(), max_lighting_spot_lights));
        u32 shadowed_spots = 0;
        for (u32 output_index = 0; output_index < spot_count; ++output_index) {
            const SpotLight &light = submission.lighting.spot_lights[spot_order[output_index]];
            const f32 range = std::max(light.range, kMinimumLightRange);
            const glm::vec3 direction = safe_normalize(light.direction, glm::vec3{0.0f, -1.0f, 0.0f});
            SpotLightGpuData &output = gpu.spot_lights[output_index];
            output.position_range = glm::vec4{light.position, range};
            output.direction_outer_cos =
                glm::vec4{direction, std::clamp(light.outer_cone_cos, 0.001f, 0.9999f)};
            output.radiance_inner_cos = glm::vec4{glm::max(light.radiance, glm::vec3{0.0f}),
                                                  std::max(light.inner_cone_cos, output.direction_outer_cos.w + 0.0001f)};
            output.shadow_params = glm::vec4{-1.0f, std::max(light.source_radius, 0.0f), 0.0f, 0.0f};
            if (!shadows_enabled || !light.casts_shadows ||
                shadowed_spots >= std::min(submission.render_graph.max_shadowed_spot_lights,
                                           max_lighting_spot_lights)) {
                continue;
            }
            const AtlasTile tile = allocator.allocate(1);
            if (!tile) {
                continue;
            }
            const f32 outer_angle = std::acos(output.direction_outer_cos.w);
            const f32 shadow_near = std::max(0.02f, range * 0.001f);
            glm::mat4 projection = glm::perspectiveRH_ZO(std::max(outer_angle * 2.0f, 0.02f), 1.0f, shadow_near, range);
            const glm::mat4 view = glm::lookAtRH(light.position, light.position + direction, light_up(direction));
            const glm::mat4 matrix = projection * view;
            const f32 radius_uv_world = std::max(light.source_radius, 0.0f) /
                                        std::max(2.0f * std::tan(outer_angle), 0.001f);
            const i32 view_index = append_shadow_view(
                matrix, tile, shadow_near, range, true, radius_uv_world,
                2.0f * std::tan(outer_angle));
            if (view_index >= 0) {
                output.shadow_params.x = static_cast<f32>(view_index);
                ++shadowed_spots;
            }
        }

        vector<usize> point_order(submission.lighting.point_lights.size());
        std::iota(point_order.begin(), point_order.end(), usize{0});
        std::stable_sort(point_order.begin(), point_order.end(), [&](usize a, usize b) {
            const PointLight &left = submission.lighting.point_lights[a];
            const PointLight &right = submission.lighting.point_lights[b];
            return punctual_importance(left.position, left.radiance, left.range, submission.camera.world_position) >
                   punctual_importance(right.position, right.radiance, right.range, submission.camera.world_position);
        });
        const u32 point_count = static_cast<u32>(std::min<usize>(point_order.size(), max_lighting_point_lights));
        u32 shadowed_points = 0;
        const array<glm::vec3, 6> face_directions{
            glm::vec3{1.0f, 0.0f, 0.0f},
            glm::vec3{-1.0f, 0.0f, 0.0f},
            glm::vec3{0.0f, 1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            glm::vec3{0.0f, 0.0f, -1.0f},
        };
        const array<glm::vec3, 6> face_ups{
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            glm::vec3{0.0f, 0.0f, -1.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
        };
        for (u32 output_index = 0; output_index < point_count; ++output_index) {
            const PointLight &light = submission.lighting.point_lights[point_order[output_index]];
            const f32 range = std::max(light.range, kMinimumLightRange);
            PointLightGpuData &output = gpu.point_lights[output_index];
            output.position_range = glm::vec4{light.position, range};
            output.radiance_source_radius = glm::vec4{glm::max(light.radiance, glm::vec3{0.0f}),
                                                      std::max(light.source_radius, 0.0f)};
            output.shadow_params = glm::vec4{-1.0f, 0.0f, 0.0f, 0.0f};
            if (!shadows_enabled || !light.casts_shadows ||
                shadowed_points >= std::min(submission.render_graph.max_shadowed_point_lights,
                                            max_shadowed_point_lights)) {
                continue;
            }

            // A point light is atomic in the budget: reserve all six cells first. If the atlas cannot
            // fit every face, none are emitted, so the shader never indexes a partial cubemap.
            AtlasAllocator candidate_allocator = allocator;
            array<AtlasTile, 6> tiles{};
            bool allocated = true;
            for (AtlasTile &tile : tiles) {
                tile = candidate_allocator.allocate(1);
                allocated &= static_cast<bool>(tile);
            }
            if (!allocated || prepared.render_views.size() + 6 > max_shadow_views) {
                continue;
            }
            allocator = candidate_allocator;
            const f32 shadow_near = std::max(0.02f, range * 0.001f);
            const glm::mat4 projection = glm::perspectiveRH_ZO(glm::half_pi<f32>(), 1.0f, shadow_near, range);
            const i32 first_view = static_cast<i32>(prepared.render_views.size());
            const f32 radius_uv_world = std::max(light.source_radius, 0.0f) * 0.5f;
            for (usize face = 0; face < face_directions.size(); ++face) {
                const glm::mat4 view = glm::lookAtRH(light.position, light.position + face_directions[face], face_ups[face]);
                (void)append_shadow_view(projection * view, tiles[face], shadow_near, range,
                                         true, radius_uv_world, 2.0f);
            }
            output.shadow_params.x = static_cast<f32>(first_view);
            ++shadowed_points;
        }

        prepared.atlas_used = !prepared.render_views.empty();
        gpu.counts = glm::vec4{static_cast<f32>(spot_count), static_cast<f32>(point_count), static_cast<f32>(prepared.render_views.size()), prepared.atlas_used ? 1.0f : 0.0f};
        const span<const ShadowLightingGpuData> data{&gpu, 1};
        auto written = device->write_buffer(targets.lighting_buffer, 0, std::as_bytes(data));
        if (!written) {
            return unexpected(graphics_error_from_rhi(written.error(), "write shadow lighting constants"));
        }
        return {};
    }

    Core::RendererResult Renderer::ensure_shadow_lighting_resources() {
        auto guard = shadow_lighting_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot build shadow lighting resources without an RHI device."));
        }

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file(
                                           "Shaders/deferred_shadow_lighting.slang",
                                           "deferred_shadow_lighting"),
                                       options);
        if (!shader) {
            return unexpected(shadow_error("compile deferred shadow lighting shader failed: " +
                                           shader.error().message + "\n" + shader.error().diagnostics));
        }
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry);
            if (!code) {
                return unexpected(shadow_error(string{"generate shadow lighting bytecode failed: "} + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), label));
            }
            return *module;
        };
        auto vertex_module = create_module(guard->vertex_entry_point, "shadow lighting vertex module");
        if (!vertex_module) {
            return unexpected(vertex_module.error());
        }
        guard->vertex_module = *vertex_module;
        auto fragment_module = create_module(guard->fragment_entry_point, "shadow lighting fragment module");
        if (!fragment_module) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(fragment_module.error());
        }
        guard->fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(
            reflection,
            reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "shadow lighting bind group layout",
            });
            if (!handle) {
                destroy_shadow_lighting_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(handle.error(), "create shadow lighting bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
        }
        if (guard->bind_group_layouts.empty()) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(shadow_error("Shadow lighting reflection produced no bind-group layout."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(),
                                                                         guard->bind_group_layouts.size()},
            .push_constant_ranges = {},
            .label = "shadow lighting pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create shadow lighting pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;

        const RHI::SamplerDesc sampler_desc{
            .min_filter = RHI::Filter::Nearest,
            .mag_filter = RHI::Filter::Nearest,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "shadow lighting nearest sampler",
        };
        auto gbuffer_sampler = device->create_sampler(sampler_desc);
        if (!gbuffer_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(gbuffer_sampler.error(), "create G-buffer sampler"));
        }
        guard->gbuffer_sampler = *gbuffer_sampler;
        auto shadow_sampler = device->create_sampler(sampler_desc);
        if (!shadow_sampler) {
            destroy_shadow_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(shadow_sampler.error(), "create shadow atlas sampler"));
        }
        guard->shadow_sampler = *shadow_sampler;
        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::shadow_lighting_pipeline_for(
        RHI::Format color_format) {
        if (Core::RendererResult ready = ensure_shadow_lighting_resources(); !ready) {
            return unexpected(ready.error());
        }
        auto guard = shadow_lighting_.lock();
        for (const ShadowLightingPipelineVariant &variant : guard->pipeline_variants) {
            if (variant.color_format == color_format) {
                return variant.pipeline;
            }
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(shadow_error("Cannot build a shadow lighting pipeline without an RHI device."));
        }
        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = false,
            .write_mask = RHI::ColorWriteMask::All,
        };
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module,
                                       .entry_point = guard->vertex_entry_point.c_str(),
                                       .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->fragment_module,
                                         .entry_point = guard->fragment_entry_point.c_str(),
                                         .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "deferred shadow lighting pipeline",
        });
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create deferred shadow lighting pipeline"));
        }
        guard->pipeline_variants.push_back(ShadowLightingPipelineVariant{
            .color_format = color_format,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    Core::RendererResult Renderer::record_shadow_lighting(
        RHI::RenderPassEncoder &pass,
        RHI::TextureViewHandle albedo_view,
        RHI::TextureViewHandle normal_view,
        RHI::TextureViewHandle material_view,
        RHI::TextureViewHandle depth_view,
        RHI::TextureViewHandle shadow_atlas_view,
        RHI::BufferHandle lighting_buffer,
        RHI::Format color_format,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        auto pipeline = shadow_lighting_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !albedo_view || !normal_view || !material_view || !depth_view ||
            !shadow_atlas_view || !lighting_buffer) {
            return unexpected(shadow_error("Deferred shadow lighting received an invalid G-buffer, atlas, or constants resource."));
        }

        auto guard = shadow_lighting_.lock();
        const vector<ReflectedResource> resources = collect_resource_bindings(guard->shader.reflection());
        if (resources.empty()) {
            return unexpected(shadow_error("Shadow lighting reflection produced no resource bindings."));
        }
        const u32 set = resources.front().set;
        vector<RHI::BindGroupEntry> entries;
        entries.reserve(resources.size());
        for (const ReflectedResource &resource : resources) {
            if (resource.set != set) {
                return unexpected(shadow_error("Shadow lighting resources unexpectedly span multiple bind groups."));
            }
            RHI::BindGroupEntry entry{.binding = resource.binding};
            if (resource.name == "lightingData") {
                entry.buffer = lighting_buffer;
                entry.size = sizeof(ShadowLightingGpuData);
            } else if (resource.name == "gbufferAlbedo") {
                entry.texture_view = albedo_view;
            } else if (resource.name == "gbufferNormal") {
                entry.texture_view = normal_view;
            } else if (resource.name == "gbufferMaterial") {
                entry.texture_view = material_view;
            } else if (resource.name == "gbufferDepth") {
                entry.texture_view = depth_view;
            } else if (resource.name == "shadowAtlas") {
                entry.texture_view = shadow_atlas_view;
            } else if (resource.name == "gbufferSampler") {
                entry.sampler = guard->gbuffer_sampler;
            } else if (resource.name == "shadowSampler") {
                entry.sampler = guard->shadow_sampler;
            } else {
                return unexpected(shadow_error("Shadow lighting reflection contains an unknown resource: " + resource.name));
            }
            entries.push_back(entry);
        }
        const usize layout_index = bind_group_layout_index_for_set(guard->bind_group_layout_sets, set);
        if (layout_index >= guard->bind_group_layouts.size()) {
            return unexpected(shadow_error("Shadow lighting bind group has no matching generated layout."));
        }
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->bind_group_layouts[layout_index],
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "deferred shadow lighting bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create deferred shadow lighting bind group"));
        }
        transient_bind_groups.push_back(*bind_group);
        pass.set_pipeline(*pipeline);
        pass.set_bind_group(set, *bind_group);
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_shadow_lighting_resources() noexcept {
        auto guard = shadow_lighting_.lock();
        destroy_shadow_lighting_resources_locked(*guard);
    }

    void Renderer::destroy_shadow_lighting_resources_locked(ShadowLightingResources &resources) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            resources = {};
            return;
        }
        for (const ShadowLightingPipelineVariant &variant : resources.pipeline_variants) {
            if (variant.pipeline) {
                device->destroy_render_pipeline(variant.pipeline);
            }
        }
        if (resources.shadow_sampler)
            device->destroy_sampler(resources.shadow_sampler);
        if (resources.gbuffer_sampler)
            device->destroy_sampler(resources.gbuffer_sampler);
        if (resources.pipeline_layout)
            device->destroy_pipeline_layout(resources.pipeline_layout);
        for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) {
            device->destroy_bind_group_layout(layout);
        }
        if (resources.fragment_module)
            device->destroy_shader_module(resources.fragment_module);
        if (resources.vertex_module)
            device->destroy_shader_module(resources.vertex_module);
        resources = {};
    }

} // namespace SFT::Renderer
