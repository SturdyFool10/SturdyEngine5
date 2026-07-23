#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError instance_cull_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        // Below this many instances, N individual CPU-recorded draws (already frustum-culled and
        // state-deduplicated by record_render_items_culled) beat the fixed overhead of a compute
        // dispatch + indirect draw for one batch — a compute shader launch, a buffer barrier, and an
        // indirect draw's extra GPU-side argument fetch are all real costs a handful of draws
        // doesn't recoup. Chosen well above that break-even point rather than tuned precisely; revisit
        // with real profiling once this path has production mileage.
        constexpr u32 kInstancedBatchMinSize = 32;

        constexpr u32 kInstanceCullWorkgroupSize = 64;

        // Same GeometryVertex input layout as RendererMaterial.cpp's (internal-linkage, TU-local)
        // geometry_vertex_attributes() — duplicated rather than shared since it's a fixed 5-attribute
        // layout unlikely to drift, and not worth promoting to a header for one extra call site.
        constexpr array<RHI::VertexAttribute, 5> instanced_geometry_vertex_attributes() {
            return {
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, position), .shader_location = 0},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x3, .offset = offsetof(GeometryVertex, normal), .shader_location = 1},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x2, .offset = offsetof(GeometryVertex, uv), .shader_location = 2},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x4, .offset = offsetof(GeometryVertex, color), .shader_location = 3},
                RHI::VertexAttribute{.format = RHI::VertexFormat::Float32x4, .offset = offsetof(GeometryVertex, tangent), .shader_location = 4},
            };
        }

    } // namespace

    vector<Renderer::InstancedBatch> Renderer::detect_instanced_batches(span<const RenderItem> sorted_draws) const {
        vector<InstancedBatch> batches;
        usize i = 0;
        while (i < sorted_draws.size()) {
            usize j = i + 1;
            while (j < sorted_draws.size() && sorted_draws[j].mesh == sorted_draws[i].mesh &&
                   sorted_draws[j].material == sorted_draws[i].material) {
                ++j;
            }
            const usize count = j - i;
            if (count >= kInstancedBatchMinSize) {
                batches.push_back(InstancedBatch{
                    .mesh = sorted_draws[i].mesh,
                    .material = sorted_draws[i].material,
                    .first_object_index = static_cast<u32>(i),
                    .instance_count = static_cast<u32>(count),
                });
            }
            i = j;
        }
        return batches;
    }

    Core::RendererResult Renderer::ensure_instance_cull_resources() {
        auto guard = instance_cull_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(instance_cull_error("Cannot build instance-cull resources without an RHI device."));
        }

        // ── Compute cull shader ──
        {
            const slang::ShaderCompileOptions options{
                .targets = {slang::ShaderTarget{}},
                .entry_points = {slang::ShaderEntryPointRequest{.name = "cullMain", .stage = slang::ShaderStage::Compute}},
            };
            slang::ShaderCompiler compiler;
            auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/gpu_instance_cull.slang", "gpu_instance_cull"), options);
            if (!shader) {
                return unexpected(instance_cull_error("compile instance-cull shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            guard->cull_shader = *shader;

            auto code = guard->cull_shader.entry_point_code("cullMain");
            if (!code) {
                return unexpected(instance_cull_error("generate instance-cull bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = "instance cull compute module",
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), "create instance cull compute module"));
            }
            guard->cull_module = *module;

            const slang::ShaderReflection &reflection = guard->cull_shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
            if (generated.empty()) {
                return unexpected(instance_cull_error("instance-cull shader reflection produced no bind-group layout."));
            }
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = "instance cull bind group layout",
            });
            if (!handle) {
                return unexpected(graphics_error_from_rhi(handle.error(), "create instance cull bind group layout"));
            }
            guard->cull_bind_group_layout = *handle;

            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
            if (push_constant_ranges.empty()) {
                return unexpected(instance_cull_error("instance-cull shader produced no push-constant range."));
            }
            const array<RHI::BindGroupLayoutHandle, 1> layouts{guard->cull_bind_group_layout};
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "instance cull pipeline layout",
            });
            if (!pipeline_layout) {
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create instance cull pipeline layout"));
            }
            guard->cull_pipeline_layout = *pipeline_layout;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = guard->cull_pipeline_layout,
                .compute = RHI::ShaderEntry{.module = guard->cull_module, .entry_point = "cullMain", .stage = RHI::ShaderStage::Compute},
                .label = "instance cull pipeline",
            });
            if (!pipeline) {
                return unexpected(graphics_error_from_rhi(pipeline.error(), "create instance cull pipeline"));
            }
            guard->cull_pipeline = *pipeline;
        }

        // ── Instanced vertex stage (Shaders/gbuffer_geometry_instanced.slang) ──
        // Compiled as its own module specifically so its reflection never touches a material
        // template's own bind-group layout — see gbuffer_geometry_instanced.slang's header comment.
        {
            const slang::ShaderCompileOptions options{
                .targets = {slang::ShaderTarget{}},
                .entry_points = {slang::ShaderEntryPointRequest{.name = "vertexMainInstanced", .stage = slang::ShaderStage::Vertex}},
            };
            slang::ShaderCompiler compiler;
            auto shader = compiler.compile(
                slang::ShaderSource::from_file("Shaders/gbuffer_geometry_instanced.slang", "gbuffer_geometry_instanced"), options);
            if (!shader) {
                return unexpected(instance_cull_error("compile instanced vertex shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            guard->instanced_vertex_shader = *shader;

            auto code = guard->instanced_vertex_shader.entry_point_code("vertexMainInstanced");
            if (!code) {
                return unexpected(instance_cull_error("generate instanced vertex bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = "instanced gbuffer vertex module",
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), "create instanced gbuffer vertex module"));
            }
            guard->instanced_vertex_module = *module;

            const slang::ShaderReflection &reflection = guard->instanced_vertex_shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Vertex);
            if (generated.empty()) {
                return unexpected(instance_cull_error("instanced vertex shader reflection produced no bind-group layout."));
            }
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = "instance data bind group layout",
            });
            if (!handle) {
                return unexpected(graphics_error_from_rhi(handle.error(), "create instance data bind group layout"));
            }
            guard->instance_data_bind_group_layout = *handle;
        }

        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::instanced_pipeline_for(
        MaterialTemplateResource &material_template, span<const RHI::Format> color_formats,
        RHI::Format depth_format, RHI::SampleCount samples) {
        if (color_formats.empty()) {
            return unexpected(instance_cull_error("Cannot build an instanced pipeline without at least one color target."));
        }
        if (Core::RendererResult ready = ensure_instance_cull_resources(); !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(instance_cull_error("Cannot build an instanced pipeline without an RHI device."));
        }

        auto templates = instanced_pipeline_variants_.lock();
        InstancedTemplateResources &template_resources = (*templates)[material_template.handle.value];

        for (const InstancedPipelineVariant &variant : template_resources.pipeline_variants) {
            if (variant.depth_format == depth_format && variant.samples == samples &&
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
                return unexpected(instance_cull_error("Material template has no set-0 bind-group layout to reuse for its instanced pipeline."));
            }
            auto guard = instance_cull_.lock();
            const array<RHI::BindGroupLayoutHandle, 2> layouts{
                material_template.bind_group_layouts[material_set0_index],
                guard->instance_data_bind_group_layout,
            };
            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(guard->instanced_vertex_shader.reflection(), RHI::ShaderStage::Vertex);
            if (push_constant_ranges.empty()) {
                return unexpected(instance_cull_error("instanced vertex shader produced no push-constant range."));
            }
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "instanced gbuffer pipeline layout",
            });
            if (!pipeline_layout) {
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create instanced gbuffer pipeline layout"));
            }
            template_resources.pipeline_layout = *pipeline_layout;
        }

        auto guard = instance_cull_.lock();
        const array<RHI::VertexAttribute, 5> attributes = instanced_geometry_vertex_attributes();
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
        // Standard (not Equal-against-a-prior-Z-prepass) depth test/write: instanced batches don't
        // run through the z-prepass (they're a separate draw stream, not in submission.draws by the
        // time the prepass records — see record_instanced_batches's doc comment), so they must
        // establish their own depth like a Z-prepass-less forward draw would.
        RHI::DepthStencilState depth_stencil{};
        if (depth_format != RHI::Format::Undefined) {
            depth_stencil = RHI::DepthStencilState{
                .format = depth_format,
                .depth_test_enable = true,
                .depth_write_enable = true,
                .depth_compare = RHI::CompareOp::Less,
            };
        }
        const RHI::RenderPipelineDesc desc{
            .layout = template_resources.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->instanced_vertex_module, .entry_point = "vertexMainInstanced", .stage = RHI::ShaderStage::Vertex},
            .fragment = material_template.has_fragment
                            ? RHI::ShaderEntry{.module = material_template.fragment_module, .entry_point = material_template.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment}
                            : RHI::ShaderEntry{},
            .vertex_buffers = span<const RHI::VertexBufferLayout>{&vertex_layout, 1},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{},
            .multisample = RHI::MultisampleState{.samples = samples},
            .depth_stencil = depth_stencil,
            .color_targets = span<const RHI::ColorTargetState>{color_targets.data(), color_targets.size()},
            .label = "instanced gbuffer pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create instanced gbuffer pipeline"));
        }
        template_resources.pipeline_variants.push_back(InstancedPipelineVariant{
            .color_formats = vector<RHI::Format>{color_formats.begin(), color_formats.end()},
            .depth_format = depth_format,
            .samples = samples,
            .pipeline = *pipeline,
        });
        return *pipeline;
    }

    Core::RendererResult Renderer::prepare_instance_cull_gpu_data(span<const InstancedBatch> batches,
                                                                   SceneFrameGpuResources &resources) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(instance_cull_error("Cannot prepare instance-cull GPU data without an RHI device."));
        }
        if (batches.empty()) {
            return {};
        }

        if (!resources.indirect_commands_buffer || resources.indirect_commands_capacity < batches.size()) {
            if (resources.indirect_commands_buffer) {
                device->destroy_buffer(resources.indirect_commands_buffer);
            }
            resources.indirect_commands_capacity = batches.size();
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = static_cast<u64>(resources.indirect_commands_capacity * sizeof(GpuDrawIndexedIndirectCommand)),
                .usage = RHI::BufferUsage::Indirect | RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "renderer instanced indirect commands buffer",
            });
            if (!buffer) {
                resources.indirect_commands_buffer = {};
                resources.indirect_commands_capacity = 0;
                return unexpected(graphics_error_from_rhi(buffer.error(), "create instanced indirect commands buffer"));
            }
            resources.indirect_commands_buffer = *buffer;
        }

        // Each batch's compacted-indices region must start at a device-aligned offset (it's bound
        // with a dynamic offset — see record_instanced_batches), so regions are padded up to
        // min_storage_buffer_offset_alignment rather than packed back-to-back.
        const u64 alignment = std::max<u64>(device->limits().min_storage_buffer_offset_alignment, sizeof(u32));
        vector<u64> compacted_indices_byte_offsets(batches.size(), 0);
        u64 required_bytes = 0;
        for (usize i = 0; i < batches.size(); ++i) {
            compacted_indices_byte_offsets[i] = required_bytes;
            const u64 region_bytes = static_cast<u64>(batches[i].instance_count) * sizeof(u32);
            required_bytes += ((region_bytes + alignment - 1) / alignment) * alignment;
        }
        const usize required_capacity_bytes = static_cast<usize>(required_bytes);
        if (!resources.compacted_indices_buffer || resources.compacted_indices_capacity < required_capacity_bytes) {
            if (resources.compacted_indices_buffer) {
                device->destroy_buffer(resources.compacted_indices_buffer);
            }
            resources.compacted_indices_capacity = std::max<usize>(required_capacity_bytes, 1);
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = static_cast<u64>(resources.compacted_indices_capacity),
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "renderer instanced compacted indices buffer",
            });
            if (!buffer) {
                resources.compacted_indices_buffer = {};
                resources.compacted_indices_capacity = 0;
                return unexpected(graphics_error_from_rhi(buffer.error(), "create instanced compacted indices buffer"));
            }
            resources.compacted_indices_buffer = *buffer;
        }

        vector<GpuDrawIndexedIndirectCommand> commands(batches.size());
        for (usize i = 0; i < batches.size(); ++i) {
            const MeshResource *mesh_resource = mesh(batches[i].mesh);
            if (mesh_resource == nullptr) {
                return unexpected(instance_cull_error("Instanced batch references an unknown mesh."));
            }
            commands[i] = GpuDrawIndexedIndirectCommand{
                .index_count = static_cast<u32>(mesh_resource->indices.size()),
                .instance_count = 0,
                .first_index = mesh_resource->index_offset,
                .vertex_offset = static_cast<i32>(mesh_resource->vertex_offset),
                .first_instance = 0,
            };
        }
        if (auto written = device->write_buffer(resources.indirect_commands_buffer, 0,
                                                std::as_bytes(span<const GpuDrawIndexedIndirectCommand>{commands.data(), commands.size()}));
            !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "upload instanced indirect commands"));
        }
        return {};
    }

    Core::RendererResult Renderer::record_instance_cull(RHI::ComputePassEncoder &pass, span<const InstancedBatch> batches,
                                                         const glm::mat4 &view_projection, SceneFrameGpuResources &resources,
                                                         vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (batches.empty()) {
            return {};
        }
        if (Core::RendererResult ready = ensure_instance_cull_resources(); !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(instance_cull_error("Cannot record instance culling without an RHI device."));
        }

        auto guard = instance_cull_.lock();
        const array<RHI::BindGroupEntry, 3> entries{
            RHI::BindGroupEntry{.binding = 0, .buffer = resources.object_buffer},
            RHI::BindGroupEntry{.binding = 1, .buffer = resources.compacted_indices_buffer},
            RHI::BindGroupEntry{.binding = 2, .buffer = resources.indirect_commands_buffer},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->cull_bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "instance cull bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create instance cull bind group"));
        }
        // Transient: destroyed by the caller's frame-in-flight retirement path, like every other
        // per-frame transient bind group — see FrameSubmission::transient_bind_groups's doc comment.
        transient_bind_groups.push_back(*bind_group);

        pass.set_pipeline(guard->cull_pipeline);
        pass.set_bind_group(0, *bind_group);

        const u64 alignment = std::max<u64>(device->limits().min_storage_buffer_offset_alignment, sizeof(u32));
        u64 compacted_byte_offset = 0;
        for (const InstancedBatch &batch : batches) {
            const MeshResource *mesh_resource = mesh(batch.mesh);
            if (mesh_resource == nullptr) {
                return unexpected(instance_cull_error("Instanced batch references an unknown mesh during cull dispatch."));
            }
            struct InstanceCullConstants {
                glm::mat4 view_projection;
                glm::vec4 bounds_center_radius;
                u32 first_object_index;
                u32 instance_count;
                u32 indirect_command_byte_offset;
                u32 compacted_indices_first_slot;
            };
            const usize batch_index = static_cast<usize>(&batch - batches.data());
            const InstanceCullConstants constants{
                .view_projection = view_projection,
                .bounds_center_radius = glm::vec4{mesh_resource->bounds_center, mesh_resource->bounds_radius},
                .first_object_index = batch.first_object_index,
                .instance_count = batch.instance_count,
                .indirect_command_byte_offset = static_cast<u32>(batch_index * sizeof(GpuDrawIndexedIndirectCommand)),
                .compacted_indices_first_slot = static_cast<u32>(compacted_byte_offset / sizeof(u32)),
            };
            pass.set_push_constants(RHI::ShaderStage::Compute, 0,
                                    std::as_bytes(span<const InstanceCullConstants>{&constants, 1}));
            const u32 group_count = (batch.instance_count + kInstanceCullWorkgroupSize - 1) / kInstanceCullWorkgroupSize;
            pass.dispatch(group_count);

            const u64 region_bytes = static_cast<u64>(batch.instance_count) * sizeof(u32);
            compacted_byte_offset += ((region_bytes + alignment - 1) / alignment) * alignment;
        }
        return {};
    }

    Core::RendererResult Renderer::record_instanced_batches(RHI::RenderPassEncoder &pass, span<const InstancedBatch> batches,
                                                             span<const RHI::Format> color_formats, RHI::Format depth_format,
                                                             u64 frame_index, const glm::mat4 &view_projection,
                                                             SceneFrameGpuResources &resources,
                                                             vector<RHI::BindGroupHandle> &transient_bind_groups,
                                                             RHI::SampleCount samples) {
        if (batches.empty()) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !vertex_arena_.buffer) {
            return unexpected(instance_cull_error("Cannot record instanced batches without a device/vertex arena."));
        }

        RHI::BindGroupLayoutHandle instance_data_layout{};
        {
            auto guard = instance_cull_.lock();
            instance_data_layout = guard->instance_data_bind_group_layout;
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = 0, .buffer = resources.object_buffer},
            RHI::BindGroupEntry{.binding = 1, .buffer = resources.compacted_indices_buffer},
        };
        auto instance_bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = instance_data_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "instance data bind group",
        });
        if (!instance_bind_group) {
            return unexpected(graphics_error_from_rhi(instance_bind_group.error(), "create instance data bind group"));
        }
        transient_bind_groups.push_back(*instance_bind_group);

        pass.set_vertex_buffer(0, vertex_arena_.buffer);
        if (index_arena_.buffer) {
            pass.set_index_buffer(index_arena_.buffer, RHI::IndexFormat::Uint32);
        }

        const u64 alignment = std::max<u64>(device->limits().min_storage_buffer_offset_alignment, sizeof(u32));
        u64 compacted_byte_offset = 0;
        RHI::RenderPipelineHandle bound_pipeline{};
        for (usize i = 0; i < batches.size(); ++i) {
            const InstancedBatch &batch = batches[i];
            MaterialInstanceResource *material_resource = material_instance(batch.material);
            if (material_resource == nullptr) {
                return unexpected(instance_cull_error("Instanced batch references an unknown material instance."));
            }
            MaterialTemplateResource *material_template_resource = material_template(material_resource->material_template);
            if (material_template_resource == nullptr) {
                return unexpected(instance_cull_error("Instanced batch material references an unknown material template."));
            }
            auto pipeline = instanced_pipeline_for(*material_template_resource, color_formats, depth_format, samples);
            if (!pipeline) {
                return unexpected(pipeline.error());
            }
            if (!(bound_pipeline == *pipeline)) {
                pass.set_pipeline(*pipeline);
                bound_pipeline = *pipeline;
            }

            const u32 frame_slot = material_resource->frames.empty()
                                        ? 0u
                                        : static_cast<u32>(frame_index % material_resource->frames.size());
            if (!material_resource->frames.empty()) {
                auto bind_groups = prepare_material_frame(*material_resource, frame_slot);
                if (!bind_groups) {
                    return unexpected(bind_groups.error());
                }
                for (usize set_index = 0; set_index < bind_groups->size() && set_index < material_template_resource->bind_group_layout_sets.size(); ++set_index) {
                    pass.set_bind_group(material_template_resource->bind_group_layout_sets[set_index], (*bind_groups)[set_index]);
                }
            }

            const u32 dynamic_offset = static_cast<u32>(compacted_byte_offset);
            pass.set_bind_group(1, *instance_bind_group, span<const u32>{&dynamic_offset, 1});

            const glm::mat4 constants = view_projection;
            pass.set_push_constants(RHI::ShaderStage::Vertex, 0, std::as_bytes(span<const glm::mat4>{&constants, 1}));

            pass.draw_indexed_indirect(resources.indirect_commands_buffer, i * sizeof(GpuDrawIndexedIndirectCommand));

            const u64 region_bytes = static_cast<u64>(batch.instance_count) * sizeof(u32);
            compacted_byte_offset += ((region_bytes + alignment - 1) / alignment) * alignment;
        }
        return {};
    }

} // namespace SFT::Renderer
