#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <Renderer/DebugDraw.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Renderer/FrameUpload.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        // Must match `DebugLineConstants` in Shaders/debug_lines.slang (push-constant / std430 layout).
        struct DebugLineConstants {
            glm::mat4 view_projection{1.0f};
            glm::vec2 viewport_size{0.0f};
            f32 occluded_alpha = 0.25f;
            f32 near_w = 0.01f;
        };
        static_assert(sizeof(DebugLineConstants) == 80);

        /// Two unit vectors perpendicular to `normal` and to each other.
        void perpendicular_basis(const glm::vec3 &normal, glm::vec3 &u, glm::vec3 &v) noexcept {
            const f32 length = glm::length(normal);
            const glm::vec3 n = length > 1.0e-6f ? normal / length : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 helper = std::abs(n.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
            u = glm::normalize(glm::cross(n, helper));
            v = glm::cross(n, u);
        }

        [[nodiscard]] Core::GraphicsBackendError debug_draw_error(std::string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    u32 pack_debug_color(const glm::vec4 &rgba) noexcept {
        const auto channel = [](f32 value) -> u32 {
            return static_cast<u32>(std::lround(std::clamp(std::isfinite(value) ? value : 0.0f, 0.0f, 1.0f) * 255.0f));
        };
        return channel(rgba.r) | (channel(rgba.g) << 8) | (channel(rgba.b) << 16) | (channel(rgba.a) << 24);
    }

    // ---- shapes -----------------------------------------------------------------------------------------------------------

    void DebugDrawList::line(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color) {
        lines_.push_back(DebugLine{.a = a, .width_px = width_px_, .b = b, .color = pack_debug_color(color)});
    }

    void DebugDrawList::polyline(std::span<const glm::vec3> points, const glm::vec4 &color, bool closed) {
        for (usize i = 1; i < points.size(); ++i) {
            line(points[i - 1], points[i], color);
        }
        if (closed && points.size() > 2) {
            line(points.back(), points.front(), color);
        }
    }

    void DebugDrawList::cross(const glm::vec3 &point, f32 size, const glm::vec4 &color) {
        const f32 h = 0.5f * size;
        line(point - glm::vec3{h, 0, 0}, point + glm::vec3{h, 0, 0}, color);
        line(point - glm::vec3{0, h, 0}, point + glm::vec3{0, h, 0}, color);
        line(point - glm::vec3{0, 0, h}, point + glm::vec3{0, 0, h}, color);
    }

    void DebugDrawList::box(const glm::vec3 &min, const glm::vec3 &max, const glm::vec4 &color) {
        oriented_box(glm::translate(glm::mat4{1.0f}, 0.5f * (min + max)), 0.5f * (max - min), color);
    }

    void DebugDrawList::oriented_box(const glm::mat4 &transform, const glm::vec3 &half_extents, const glm::vec4 &color) {
        glm::vec3 corners[8];
        for (u32 i = 0; i < 8; ++i) {
            const glm::vec3 local{(i & 1u) ? half_extents.x : -half_extents.x, (i & 2u) ? half_extents.y : -half_extents.y,
                                  (i & 4u) ? half_extents.z : -half_extents.z};
            corners[i] = glm::vec3{transform * glm::vec4{local, 1.0f}};
        }
        // Corners differ in one bit per edge: connect every pair that does.
        for (u32 i = 0; i < 8; ++i) {
            for (u32 bit = 1; bit <= 4; bit <<= 1) {
                if ((i & bit) == 0) {
                    line(corners[i], corners[i | bit], color);
                }
            }
        }
    }

    void DebugDrawList::arc(const glm::vec3 &center, const glm::vec3 &axis_x, const glm::vec3 &axis_y, f32 radius, f32 start_radians,
                            f32 end_radians, const glm::vec4 &color, u32 segments) {
        segments = std::max(segments, 1u);
        glm::vec3 previous = center + radius * (std::cos(start_radians) * axis_x + std::sin(start_radians) * axis_y);
        for (u32 i = 1; i <= segments; ++i) {
            const f32 angle = start_radians + (end_radians - start_radians) * static_cast<f32>(i) / static_cast<f32>(segments);
            const glm::vec3 point = center + radius * (std::cos(angle) * axis_x + std::sin(angle) * axis_y);
            line(previous, point, color);
            previous = point;
        }
    }

    void DebugDrawList::circle(const glm::vec3 &center, const glm::vec3 &normal, f32 radius, const glm::vec4 &color, u32 segments) {
        glm::vec3 u, v;
        perpendicular_basis(normal, u, v);
        arc(center, u, v, radius, 0.0f, glm::two_pi<f32>(), color, std::max(segments, 3u));
    }

    void DebugDrawList::sphere(const glm::vec3 &center, f32 radius, const glm::vec4 &color, u32 segments) {
        circle(center, glm::vec3{1, 0, 0}, radius, color, segments);
        circle(center, glm::vec3{0, 1, 0}, radius, color, segments);
        circle(center, glm::vec3{0, 0, 1}, radius, color, segments);
    }

    void DebugDrawList::capsule(const glm::vec3 &a, const glm::vec3 &b, f32 radius, const glm::vec4 &color, u32 segments) {
        const glm::vec3 axis = b - a;
        const f32 length = glm::length(axis);
        if (length < 1.0e-6f) {
            sphere(a, radius, color, segments * 2);
            return;
        }
        const glm::vec3 direction = axis / length;
        glm::vec3 u, v;
        perpendicular_basis(direction, u, v);
        const glm::vec3 side[4] = {u, v, -u, -v};
        for (const glm::vec3 &s : side) {
            line(a + s * radius, b + s * radius, color);
        }
        circle(a, direction, radius, color, segments * 2);
        circle(b, direction, radius, color, segments * 2);
        // Hemispheres: a half circle in each of the two planes containing the axis, bulging away from the segment.
        const f32 half = glm::pi<f32>();
        arc(a, u, -direction, radius, 0.0f, half, color, segments);
        arc(a, v, -direction, radius, 0.0f, half, color, segments);
        arc(b, u, direction, radius, 0.0f, half, color, segments);
        arc(b, v, direction, radius, 0.0f, half, color, segments);
    }

    void DebugDrawList::cone(const glm::vec3 &apex, const glm::vec3 &direction, f32 length, f32 half_angle_radians, const glm::vec4 &color,
                             u32 segments) {
        const f32 direction_length = glm::length(direction);
        const glm::vec3 d = direction_length > 1.0e-6f ? direction / direction_length : glm::vec3{0.0f, -1.0f, 0.0f};
        const f32 radius = length * std::tan(std::min(half_angle_radians, glm::half_pi<f32>() - 0.01f));
        const glm::vec3 base = apex + d * length;
        glm::vec3 u, v;
        perpendicular_basis(d, u, v);
        circle(base, d, radius, color, segments);
        const glm::vec3 spokes[4] = {u, v, -u, -v};
        for (const glm::vec3 &s : spokes) {
            line(apex, base + s * radius, color);
        }
        line(apex, base, color);
    }

    void DebugDrawList::arrow(const glm::vec3 &from, const glm::vec3 &to, const glm::vec4 &color, f32 head_size) {
        line(from, to, color);
        const glm::vec3 axis = to - from;
        const f32 length = glm::length(axis);
        if (length < 1.0e-6f) {
            return;
        }
        const glm::vec3 d = axis / length;
        const f32 head = head_size > 0.0f ? head_size : 0.2f * length;
        glm::vec3 u, v;
        perpendicular_basis(d, u, v);
        const glm::vec3 base = to - d * head;
        const glm::vec3 wings[4] = {u, v, -u, -v};
        for (const glm::vec3 &w : wings) {
            line(to, base + w * (0.4f * head), color);
        }
    }

    void DebugDrawList::axes(const glm::mat4 &transform, f32 size) {
        const glm::vec3 origin{transform[3]};
        line(origin, origin + glm::vec3{transform[0]} * size, glm::vec4{1.0f, 0.15f, 0.15f, 1.0f});
        line(origin, origin + glm::vec3{transform[1]} * size, glm::vec4{0.2f, 1.0f, 0.2f, 1.0f});
        line(origin, origin + glm::vec3{transform[2]} * size, glm::vec4{0.25f, 0.45f, 1.0f, 1.0f});
    }

    void DebugDrawList::frustum(const glm::mat4 &inverse_view_projection, const glm::vec4 &color) {
        glm::vec3 corners[8];
        for (u32 i = 0; i < 8; ++i) {
            const glm::vec4 ndc{(i & 1u) ? 1.0f : -1.0f, (i & 2u) ? 1.0f : -1.0f, (i & 4u) ? 1.0f : 0.0f, 1.0f};
            const glm::vec4 world = inverse_view_projection * ndc;
            corners[i] = glm::vec3{world} / (std::abs(world.w) > 1.0e-12f ? world.w : 1.0f);
        }
        for (u32 i = 0; i < 8; ++i) {
            for (u32 bit = 1; bit <= 4; bit <<= 1) {
                if ((i & bit) == 0) {
                    line(corners[i], corners[i | bit], color);
                }
            }
        }
    }

    void DebugDrawList::grid(const glm::vec3 &center, const glm::vec3 &axis_u, const glm::vec3 &axis_v, u32 half_cells, f32 cell_size,
                             const glm::vec4 &color) {
        const f32 extent = static_cast<f32>(half_cells) * cell_size;
        for (i32 i = -static_cast<i32>(half_cells); i <= static_cast<i32>(half_cells); ++i) {
            const f32 offset = static_cast<f32>(i) * cell_size;
            line(center + axis_u * offset - axis_v * extent, center + axis_u * offset + axis_v * extent, color);
            line(center + axis_v * offset - axis_u * extent, center + axis_v * offset + axis_u * extent, color);
        }
    }

    // ---- renderer ---------------------------------------------------------------------------------------------------------

    /// What one frame's overlay shares between its `prepare` (graph build) and `draw` (graph execution).
    struct DebugDrawRenderer::FrameState {
        std::shared_ptr<const std::vector<DebugLine>> lines;
        glm::mat4 view_projection{1.0f};
        f32 near_plane = 0.01f;
        DebugDrawSettings settings{};
        RHI::RhiDevice *device = nullptr;
        /// The frame's list of bind groups to free once it has finished. Overlay passes run after tone mapping and depend
        /// on its output, so nothing else records into this list while an overlay draws.
        std::vector<RHI::BindGroupHandle> *transient_bind_groups = nullptr;
        RHI::BufferHandle buffer{};
        u32 count = 0;
        RenderGraphTextureHandle depth{};
    };

    void DebugDrawRenderer::destroy_pipeline(RHI::RhiDevice &device, Pipeline &pipeline) noexcept {
        if (pipeline.pipeline) device.destroy_render_pipeline(pipeline.pipeline);
        if (pipeline.pipeline_layout) device.destroy_pipeline_layout(pipeline.pipeline_layout);
        if (pipeline.bind_group_layout) device.destroy_bind_group_layout(pipeline.bind_group_layout);
        if (pipeline.fragment_module) device.destroy_shader_module(pipeline.fragment_module);
        if (pipeline.vertex_module) device.destroy_shader_module(pipeline.vertex_module);
        pipeline = Pipeline{};
    }

    void DebugDrawRenderer::release() noexcept {
        auto pipelines = pipelines_.lock();
        if (device_ != nullptr) {
            for (auto &entry : *pipelines) {
                destroy_pipeline(*device_, *entry.second);
            }
        }
        pipelines->clear();
        device_ = nullptr;
    }

    Core::RendererExpected<const DebugDrawRenderer::Pipeline *> DebugDrawRenderer::pipeline_for(RHI::RhiDevice &device, RHI::Format format,
                                                                                               bool with_depth) {
        const u64 key = (static_cast<u64>(format) << 1) | (with_depth ? 1u : 0u);
        auto pipelines = pipelines_.lock();
        if (const auto found = pipelines->find(key); found != pipelines->end()) {
            return found->second.get();
        }

        const auto shader_target = shader_target_for_device(device);
        if (!shader_target) {
            return std::unexpected(shader_target.error());
        }
        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
            .macros = {slang::ShaderMacro{.name = "SFT_DEBUG_LINES_DEPTH", .value = with_depth ? "1" : "0"}},
        };
        slang::ShaderVariantCache shader_cache{slang::ShaderSource::from_file("Shaders/debug_lines.slang", "debug_lines"), options,
                                               slang::ShaderCompiler{}, true};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return std::unexpected(debug_draw_error("compile debug_lines shader failed: " + shader.error().message + "\n" +
                                                    shader.error().diagnostics));
        }

        auto built = std::make_unique<Pipeline>();
        Pipeline &pipeline = *built;
        const auto fail = [&](Core::GraphicsBackendError error) -> Core::RendererExpected<const Pipeline *> {
            destroy_pipeline(device, pipeline);
            return std::unexpected(std::move(error));
        };

        auto vertex_code = shader->entry_point_code("vertexMain", shader_target->slang_target.format);
        if (!vertex_code) {
            return fail(debug_draw_error("generate debug_lines vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = std::span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "debug lines vertex module",
        });
        if (!vertex_module) {
            return fail(graphics_error_from_rhi(vertex_module.error(), "create debug lines vertex module"));
        }
        pipeline.vertex_module = *vertex_module;

        auto fragment_code = shader->entry_point_code("fragmentMain", shader_target->slang_target.format);
        if (!fragment_code) {
            return fail(debug_draw_error("generate debug_lines fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = std::span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "debug lines fragment module",
        });
        if (!fragment_module) {
            return fail(graphics_error_from_rhi(fragment_module.error(), "create debug lines fragment module"));
        }
        pipeline.fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = shader->reflection();
        const RHI::ShaderStage stage_mask = reflected_stage_mask(reflection);
        const std::vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, stage_mask);
        if (generated.size() != 1) {
            return fail(debug_draw_error("debug_lines is expected to declare all of its resources in one bind group."));
        }
        auto layout = device.create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = std::span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
            .label = "debug lines bind group layout",
        });
        if (!layout) {
            return fail(graphics_error_from_rhi(layout.error(), "create debug lines bind group layout"));
        }
        pipeline.bind_group_layout = *layout;
        pipeline.bind_group_set = generated.front().set;

        bool found_lines = false;
        for (const ReflectedResource &resource : collect_resource_bindings(reflection)) {
            if (resource.set != pipeline.bind_group_set) {
                continue;
            }
            if (resource.name == "lines") {
                pipeline.lines_binding = resource.binding;
                found_lines = true;
            } else if (resource.name == "sceneDepth") {
                pipeline.depth_binding = resource.binding;
                pipeline.has_depth_binding = true;
            }
        }
        if (!found_lines || pipeline.has_depth_binding != with_depth) {
            return fail(debug_draw_error("debug_lines shader reflection did not produce the expected bindings."));
        }

        const std::vector<RHI::PushConstantRange> push_ranges =
            generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment);
        if (push_ranges.empty()) {
            return fail(debug_draw_error("debug_lines shader reflection did not produce its push-constant range."));
        }
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = std::span<const RHI::BindGroupLayoutHandle>{&pipeline.bind_group_layout, 1},
            .push_constant_ranges = std::span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "debug lines pipeline layout",
        });
        if (!pipeline_layout) {
            return fail(graphics_error_from_rhi(pipeline_layout.error(), "create debug lines pipeline layout"));
        }
        pipeline.pipeline_layout = *pipeline_layout;

        const RHI::ColorTargetState color_target{
            .format = format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::SrcAlpha, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha,
                                         .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha,
                                         .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        auto rhi_pipeline = device.create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = pipeline.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = pipeline.vertex_module, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = pipeline.fragment_module, .entry_point = "fragmentMain",
                                         .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = std::span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "debug lines pipeline",
        });
        if (!rhi_pipeline) {
            return fail(graphics_error_from_rhi(rhi_pipeline.error(), "create debug lines pipeline"));
        }
        pipeline.pipeline = *rhi_pipeline;

        device_ = &device;
        const Pipeline *result = built.get();
        pipelines->emplace(key, std::move(built));
        return result;
    }

    OverlayPass DebugDrawRenderer::make_overlay(std::shared_ptr<const std::vector<DebugLine>> lines, const glm::mat4 &view_projection,
                                                f32 near_plane, const DebugDrawSettings &settings) {
        auto state = std::make_shared<FrameState>();
        state->lines = std::move(lines);
        state->view_projection = view_projection;
        state->near_plane = near_plane;
        state->settings = settings;

        OverlayPass overlay;
        overlay.name = "debug draw";
        overlay.prepare = [this, state](OverlayPrepareContext &context) -> Core::RendererResult {
            const usize available = state->lines ? state->lines->size() : 0;
            state->count = static_cast<u32>(std::min<usize>(available, state->settings.max_lines));
            if (state->count == 0) {
                return {};
            }
            {
                // A different device (backend switch, device recovery) invalidated every pipeline built for the old one, and
                // the old device may be gone already: forget the handles rather than destroy them through it.
                auto pipelines = pipelines_.lock();
                if (device_ != nullptr && device_ != &context.device) {
                    pipelines->clear();
                }
                device_ = &context.device;
            }
            state->device = &context.device;
            state->transient_bind_groups = &context.transient_bind_groups;

            auto buffer = upload_transient_buffer(
                context.device, context.transient_buffers, RHI::BufferUsage::Storage,
                std::as_bytes(std::span<const DebugLine>{state->lines->data(), state->count}), "debug lines");
            if (!buffer) {
                return std::unexpected(buffer.error());
            }
            state->buffer = *buffer;

            if (state->settings.depth_test) {
                const RenderGraphTextureHandle depth = context.resources.texture<RenderGraphSemantics::ResolvedSceneDepth>();
                if (depth) {
                    state->depth = depth;
                    context.sampled_textures.push_back(depth);
                }
            }
            return {};
        };
        overlay.draw = [this, state](OverlayPassContext &context) -> Core::RendererResult {
            if (state->count == 0 || state->device == nullptr) {
                return {};
            }
            const bool with_depth = static_cast<bool>(state->depth);
            auto pipeline = pipeline_for(*state->device, context.format, with_depth);
            if (!pipeline) {
                return std::unexpected(pipeline.error());
            }
            const Pipeline &p = **pipeline;

            std::vector<RHI::BindGroupEntry> entries;
            entries.push_back(RHI::BindGroupEntry{.binding = p.lines_binding, .buffer = state->buffer, .offset = 0, .size = 0,
                                                  .structure_stride = sizeof(DebugLine)});
            if (with_depth) {
                entries.push_back(RHI::BindGroupEntry{.binding = p.depth_binding,
                                                      .texture_view = context.graph.texture(state->depth).default_view});
            }
            auto bind_group = state->device->create_bind_group(RHI::BindGroupDesc{
                .layout = p.bind_group_layout,
                .entries = std::span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                .lifetime = RHI::BindGroupLifetime::FrameTransient,
                .label = "debug lines bind group",
            });
            if (!bind_group) {
                return std::unexpected(graphics_error_from_rhi(bind_group.error(), "create debug lines bind group"));
            }
            if (state->transient_bind_groups != nullptr) {
                state->transient_bind_groups->push_back(*bind_group);
            }

            const DebugLineConstants constants{
                .view_projection = state->view_projection,
                .viewport_size = glm::vec2{static_cast<f32>(context.extent.x), static_cast<f32>(context.extent.y)},
                .occluded_alpha = state->settings.occluded_alpha,
                .near_w = std::max(state->near_plane, 1.0e-4f),
            };
            context.pass.set_pipeline(p.pipeline);
            context.pass.set_bind_group(p.bind_group_set, *bind_group);
            context.pass.set_push_constants(RHI::ShaderStage::Vertex | RHI::ShaderStage::Fragment, 0,
                                            std::as_bytes(std::span<const DebugLineConstants>{&constants, 1}));
            context.pass.draw(RHI::DrawArgs{.vertex_count = 6, .instance_count = state->count, .first_vertex = 0, .first_instance = 0});
            return {};
        };
        return overlay;
    }

} // namespace SFT::Renderer
