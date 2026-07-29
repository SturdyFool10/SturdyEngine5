#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <array>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/Scene.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError object_history_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        // Same GeometryVertex input layout as RendererMaterial.cpp's/RendererGpuCulling.cpp's own
        // (internal-linkage, TU-local) copies — duplicated rather than shared for the same reason
        // those two don't share one either (a fixed 5-attribute layout unlikely to drift).
        constexpr array<RHI::VertexAttribute, 5> history_geometry_vertex_attributes() {
            return {
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, position), .shader_location = 0},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, normal), .shader_location = 1},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x2, .offset = offsetof(GeometryVertex, uv), .shader_location = 2},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x4, .offset = offsetof(GeometryVertex, color), .shader_location = 3},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x4, .offset = offsetof(GeometryVertex, tangent), .shader_location = 4},
            };
        }

    } // namespace

    Core::RendererResult Renderer::ensure_object_history_resources() {
        auto guard = object_history_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(object_history_error("Cannot build object-history resources without an RHI device."));
        }

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {slang::ShaderEntryPointRequest{.name = "vertexMainWithHistory", .stage = slang::ShaderStage::Vertex}},
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(
            slang::ShaderSource::from_file("Shaders/gbuffer_geometry_history.slang", "gbuffer_geometry_history"), options);
        if (!shader) {
            return unexpected(object_history_error("compile object-history vertex shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        guard->vertex_shader = *shader;

        auto code = guard->vertex_shader.entry_point_code("vertexMainWithHistory");
        if (!code) {
            return unexpected(object_history_error("generate object-history vertex bytecode failed: " + code.error().message));
        }
        auto module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
            .label = "object history gbuffer vertex module",
        });
        if (!module) {
            return unexpected(graphics_error_from_rhi(module.error(), "create object history gbuffer vertex module"));
        }
        guard->vertex_module = *module;

        const slang::ShaderReflection &reflection = guard->vertex_shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Vertex);
        if (generated.empty()) {
            return unexpected(object_history_error("object-history vertex shader reflection produced no bind-group layout."));
        }
        auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
            .label = "object history bind group layout",
        });
        if (!handle) {
            return unexpected(graphics_error_from_rhi(handle.error(), "create object history bind group layout"));
        }
        guard->bind_group_layout = *handle;

        guard->vertex_shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::history_pipeline_for(
        MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
        RHI::Format depth_format, bool standard_depth_test, RHI::SampleCount samples) {
        if (color_formats.empty()) {
            return unexpected(object_history_error("Cannot build a history pipeline without at least one color target."));
        }
        if (Core::RendererResult ready = ensure_object_history_resources(); !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(object_history_error("Cannot build a history pipeline without an RHI device."));
        }

        auto templates = object_history_pipeline_variants_.lock();
        ObjectHistoryTemplateResources &template_resources = (*templates)[material_template.handle.value];

        for (const ObjectHistoryPipelineVariant &variant : template_resources.pipeline_variants) {
            if (variant.depth_format == depth_format &&
                variant.standard_depth_test == standard_depth_test && variant.samples == samples &&
                variant.color_formats.size() == color_formats.size() &&
                std::equal(variant.color_formats.begin(), variant.color_formats.end(), color_formats.begin())) {
                return variant.pipeline;
            }
        }

        if (!template_resources.pipeline_layout) {
            u32 material_set0_index = material_template.bind_group_layout_sets.size();
            for (usize i = 0; i < material_template.bind_group_layout_sets.size(); ++i) {
                if (material_template.bind_group_layout_sets[i] == 0) {
                    material_set0_index = static_cast<u32>(i);
                    break;
                }
            }
            if (material_set0_index >= material_template.bind_group_layouts.size()) {
                return unexpected(object_history_error("Material template has no set-0 bind-group layout to reuse for its history pipeline."));
            }
            auto guard = object_history_.lock();
            const array<RHI::BindGroupLayoutHandle, 2> layouts{
                material_template.bind_group_layouts[material_set0_index],
                guard->bind_group_layout,
            };
            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(guard->vertex_shader.reflection(), RHI::ShaderStage::Vertex);
            if (push_constant_ranges.empty()) {
                return unexpected(object_history_error("object-history vertex shader produced no push-constant range."));
            }
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "object history gbuffer pipeline layout",
            });
            if (!pipeline_layout) {
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create object history gbuffer pipeline layout"));
            }
            template_resources.pipeline_layout = *pipeline_layout;
        }

        auto guard = object_history_.lock();
        const array<RHI::VertexAttribute, 5> attributes = history_geometry_vertex_attributes();
        const RHI::VertexBufferLayout vertex_layout{
            .stride = sizeof(GeometryVertex),
            .step_mode = RHI::VertexStepMode::Vertex,
            .attributes = span<const RHI::VertexAttribute>{attributes.data(), attributes.size()},
        };
        vector<RHI::ColorTargetState> color_targets;
        color_targets.reserve(color_formats.size());
        for (RHI::Format color_format : color_formats) {
            color_targets.push_back(RHI::ColorTargetState{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All});
        }
        // In the conventional 1x path the Z prepass populated this same depth target, so the
        // history-aware G-buffer pipeline can use Equal/no-write. SRAA instead writes its visibility
        // prepass into a separate MSAA depth image; its 1x G-buffer must establish ordinary depth.
        RHI::DepthStencilState depth_stencil{};
        if (depth_format != RHI::Format::Undefined) {
            depth_stencil = standard_depth_test
                ? RHI::DepthStencilState{
                      .format = depth_format,
                      .depth_test_enable = true,
                      .depth_write_enable = true,
                      .depth_compare = RHI::CompareOp::Less,
                  }
                : RHI::DepthStencilState{
                      .format = depth_format,
                      .depth_test_enable = true,
                      .depth_write_enable = false,
                      .depth_compare = RHI::CompareOp::Equal,
                  };
        }
        const RHI::RenderPipelineDesc desc{
            .layout = template_resources.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = "vertexMainWithHistory", .stage = RHI::ShaderStage::Vertex},
            .fragment = material_template.has_fragment
                            ? RHI::ShaderEntry{.module = material_template.fragment_module, .entry_point = material_template.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment}
                            : RHI::ShaderEntry{},
            .vertex_buffers = span<const RHI::VertexBufferLayout>{&vertex_layout, 1},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{},
            .multisample = RHI::MultisampleState{.samples = samples},
            .depth_stencil = depth_stencil,
            .color_targets = span<const RHI::ColorTargetState>{color_targets.data(), color_targets.size()},
            .label = "object history gbuffer pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create object history gbuffer pipeline"));
        }
        template_resources.pipeline_variants.push_back(ObjectHistoryPipelineVariant{
            .color_formats = vector<RHI::Format>{color_formats.begin(), color_formats.end()},
            .depth_format = depth_format,
            .standard_depth_test = standard_depth_test,
            .samples = samples,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    Core::RendererExpected<RHI::BindGroupHandle> Renderer::ensure_object_history_bind_group(
        SceneFrameGpuResources &resources, vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (Core::RendererResult ready = ensure_object_history_resources(); !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(object_history_error("Cannot build an object history bind group without an RHI device."));
        }
        RHI::BindGroupLayoutHandle layout{};
        {
            auto guard = object_history_.lock();
            layout = guard->bind_group_layout;
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = 0, .buffer = resources.object_buffer},
            RHI::BindGroupEntry{.binding = 1, .buffer = resources.view_buffer},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "object history bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create object history bind group"));
        }
        // See transient_bind_groups_lock_'s own doc comment (RendererModule.hpp). This particular
        // caller (render_frame_rhi) only ever calls this once, before the render graph is built, so
        // it isn't currently reachable concurrently — locked anyway so that invariant isn't silently
        // required of every future caller.
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }
        return *bind_group;
    }

    void Renderer::destroy_object_history_resources() noexcept {
        RHI::RhiDevice *device = rhi_device();
        auto templates = object_history_pipeline_variants_.lock();
        if (device != nullptr) {
            for (auto &[_, resources] : *templates) {
                for (const ObjectHistoryPipelineVariant &variant : resources.pipeline_variants) {
                    if (variant.pipeline) {
                        device->destroy_render_pipeline(variant.pipeline);
                    }
                }
                if (resources.pipeline_layout) {
                    device->destroy_pipeline_layout(resources.pipeline_layout);
                }
            }
        }
        templates->clear();

        auto resources = object_history_.lock();
        if (device != nullptr) {
            if (resources->bind_group_layout) {
                device->destroy_bind_group_layout(resources->bind_group_layout);
            }
            if (resources->vertex_module) {
                device->destroy_shader_module(resources->vertex_module);
            }
        }
        *resources = {};
    }

} // namespace SFT::Renderer
