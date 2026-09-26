#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <Async/Async.hpp>
#pragma endregion

#include <Renderer/Displacement/DisplacementMesh.hpp>
#include <Renderer/Displacement/DisplacementPlanner.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

// Mesh-shader geometry path for displaced materials: pipeline creation, per-draw constants and the draw
// itself. See DisplacementMeshPath.hpp and Shaders/displacement_mesh.slang. The selection seam is
// enable_displacement_mesh_geometry(): a material template that has been enabled draws through
// task + mesh + fragment stages; every other template keeps its vertex path untouched, so "fall back to
// the legacy path" is simply "do not enable" (or disable again).
namespace SFT::Renderer {

    namespace {

        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError mesh_path_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        constexpr RHI::ShaderStage kMeshStages = RHI::ShaderStage::Task | RHI::ShaderStage::Mesh;

        // Largest task-stage group count a single dispatch may use on every conformant device
        // (maxTaskWorkGroupCount[0] guarantees at least 65535). Bigger base meshes are split.
        constexpr u32 kMaxGroupsPerDraw = 65535;

        [[nodiscard]] u32 align_up(u32 value, u32 alignment) noexcept {
            return alignment == 0 ? value : ((value + alignment - 1u) / alignment) * alignment;
        }

    } // namespace

    bool Renderer::displacement_mesh_geometry_supported() const {
        const RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return false;
        }
        // Vulkan only for now: the D3D12 backend's register-space mapping for the set-1 resources and the
        // Metal/WebGPU stories are not verified (WGSL has no mesh shaders at all).
        if (device->backend_type() != RHI::BackendType::Vulkan) {
            return false;
        }
        if (!device->is_enabled(RHI::Feature::MeshShader) || !device->is_enabled(RHI::Feature::TaskShader)) {
            return false;
        }
        const Displacement::DisplacementCapabilities caps =
            Displacement::DisplacementCapabilities::from_rhi(device->enabled_features(), device->feature_properties());
        return Displacement::max_mesh_shader_subdivision(caps) > 0;
    }

    bool Renderer::displacement_mesh_geometry_active(MaterialTemplateHandle handle) const {
        if (displacement_mesh_template_count_.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        return displacement_mesh_.lock()->templates.contains(handle.value);
    }

    Core::RendererResult Renderer::enable_displacement_mesh_geometry(MaterialTemplateHandle handle,
                                                                     DisplacementMeshSettings settings) {
        ZoneScopedN("Renderer::enable_displacement_mesh_geometry");
        if (!displacement_mesh_geometry_supported()) {
            return unexpected(mesh_path_error("The mesh-shader displacement path needs Vulkan with mesh + task shaders."));
        }
        const MaterialTemplateResource *tmpl = material_template(handle);
        if (tmpl == nullptr) {
            return unexpected(mesh_path_error("enable_displacement_mesh_geometry: unknown material template."));
        }

        RHI::RhiDevice *device = rhi_device();
        const Displacement::DisplacementCapabilities caps =
            Displacement::DisplacementCapabilities::from_rhi(device->enabled_features(), device->feature_properties());
        settings.max_level = std::clamp(settings.max_level, 1u,
                                        std::max(1u, Displacement::max_mesh_shader_subdivision(caps)));
        // The geometry carries the displacement, so the per-pixel block must be NormalOnly (see
        // displacement_mesh.slang). Replace whatever the caller's macro list said.
        std::erase_if(settings.macros, [](const slang::ShaderMacro &m) { return m.name == "SFT_HF_ALGORITHM"; });
        settings.macros.push_back({"SFT_HF_ALGORITHM", "0"});

        {
            auto resources = displacement_mesh_.lock();
            DisplacementMeshTemplateState state{};
            state.settings = std::move(settings);
            const bool inserted = resources->templates.insert_or_assign(handle.value, std::move(state)).second;
            if (inserted) {
                displacement_mesh_template_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (Core::RendererResult built = ensure_displacement_mesh_built(*const_cast<MaterialTemplateResource *>(tmpl));
            !built.has_value()) {
            disable_displacement_mesh_geometry(handle);
            return built;
        }
        return {};
    }

    void Renderer::disable_displacement_mesh_geometry(MaterialTemplateHandle handle) noexcept {
        RHI::RhiDevice *device = rhi_device();
        auto resources = displacement_mesh_.lock();
        const auto found = resources->templates.find(handle.value);
        if (found == resources->templates.end()) {
            return;
        }
        if (device != nullptr) {
            device->wait_idle();
            DisplacementMeshTemplateState &state = found->second;
            for (const DisplacementMeshPipelineVariant &variant : state.variants) {
                if (variant.pipeline) device->destroy_render_pipeline(variant.pipeline);
            }
            if (state.pipeline_layout) device->destroy_pipeline_layout(state.pipeline_layout);
            if (state.task_module) device->destroy_shader_module(state.task_module);
            if (state.mesh_module) device->destroy_shader_module(state.mesh_module);
            if (state.fragment_module) device->destroy_shader_module(state.fragment_module);
            if (state.depth_only_fragment_module) device->destroy_shader_module(state.depth_only_fragment_module);
        }
        resources->templates.erase(found);
        displacement_mesh_template_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    void Renderer::invalidate_displacement_mesh_gpu(MaterialTemplateHandle handle) noexcept {
        RHI::RhiDevice *device = rhi_device();
        auto resources = displacement_mesh_.lock();
        const auto found = resources->templates.find(handle.value);
        if (found == resources->templates.end()) {
            return;
        }
        DisplacementMeshTemplateState &state = found->second;
        if (device != nullptr) {
            for (const DisplacementMeshPipelineVariant &variant : state.variants) {
                if (variant.pipeline) device->destroy_render_pipeline(variant.pipeline);
            }
            if (state.pipeline_layout) device->destroy_pipeline_layout(state.pipeline_layout);
            if (state.task_module) device->destroy_shader_module(state.task_module);
            if (state.mesh_module) device->destroy_shader_module(state.mesh_module);
            if (state.fragment_module) device->destroy_shader_module(state.fragment_module);
            if (state.depth_only_fragment_module) device->destroy_shader_module(state.depth_only_fragment_module);
        }
        DisplacementMeshTemplateState fresh{};
        fresh.settings = std::move(state.settings);
        state = std::move(fresh); // built == false: rebuilt against the new material layout on the next draw
    }

    void Renderer::destroy_displacement_mesh_resources() noexcept {
        RHI::RhiDevice *device = rhi_device();
        auto resources = displacement_mesh_.lock();
        if (device != nullptr) {
            for (auto &[_, state] : resources->templates) {
                for (const DisplacementMeshPipelineVariant &variant : state.variants) {
                    if (variant.pipeline) device->destroy_render_pipeline(variant.pipeline);
                }
                if (state.pipeline_layout) device->destroy_pipeline_layout(state.pipeline_layout);
                if (state.task_module) device->destroy_shader_module(state.task_module);
                if (state.mesh_module) device->destroy_shader_module(state.mesh_module);
                if (state.fragment_module) device->destroy_shader_module(state.fragment_module);
                if (state.depth_only_fragment_module) device->destroy_shader_module(state.depth_only_fragment_module);
            }
            if (resources->draw_layout) device->destroy_bind_group_layout(resources->draw_layout);
        }
        resources->templates.clear();
        resources->draw_layout = {};
        displacement_mesh_template_count_.store(0, std::memory_order_relaxed);
    }

    void Renderer::reset_displacement_mesh_gpu_after_device_loss() noexcept {
        // The device is gone: handles are dead, so drop them without destroying. Settings stay; the GPU
        // objects are rebuilt lazily by ensure_displacement_mesh_built on the next draw.
        auto resources = displacement_mesh_.lock();
        for (auto &[_, state] : resources->templates) {
            DisplacementMeshTemplateState fresh{};
            fresh.settings = std::move(state.settings);
            state = std::move(fresh);
        }
        resources->draw_layout = {};
    }

    Core::RendererResult Renderer::ensure_displacement_mesh_built(MaterialTemplateResource &tmpl) {
        ZoneScopedN("Renderer::ensure_displacement_mesh_built");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(mesh_path_error("Cannot build the displacement mesh path without an RHI device."));
        }
        auto resources = displacement_mesh_.lock();
        const auto found = resources->templates.find(tmpl.handle.value);
        if (found == resources->templates.end()) {
            return unexpected(mesh_path_error("Displacement mesh path is not enabled for this material template."));
        }
        DisplacementMeshTemplateState &state = found->second;
        if (state.built) {
            return {};
        }

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) {
            return unexpected(shader_target.error());
        }

        // ---- Compile ------------------------------------------------------------------------------------
        slang::ShaderCompileOptions options{};
        options.targets = shader_compile_targets_for_device(*device);
        options.macros = state.settings.macros;
        options.entry_points = {
            slang::ShaderEntryPointRequest{.name = "taskMain", .stage = slang::ShaderStage::Amplification},
            slang::ShaderEntryPointRequest{.name = "meshMain", .stage = slang::ShaderStage::Mesh},
            slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            slang::ShaderEntryPointRequest{.name = "depthOnlyMain", .stage = slang::ShaderStage::Fragment},
        };
        slang::ShaderVariantCache cache{
            slang::ShaderSource::from_file(state.settings.shader_path, state.settings.module_name), options,
            slang::ShaderCompiler{}, recovery_create_info_.enable_shader_disk_cache};
        auto compiled = cache.get_or_compile_base();
        if (!compiled) {
            return unexpected(mesh_path_error("compile displacement mesh shader failed: " + compiled.error().message +
                                              "\n" + compiled.error().diagnostics));
        }
        slang::Shader shader = *compiled;
        const slang::ShaderReflection &reflection = shader.reflection();

        // ---- Layout: material set 0 must be exactly the template's (the vertex path's) ----------------------
        const vector<GeneratedBindGroupLayout> generated =
            generate_bind_group_layouts(reflection, kMeshStages | RHI::ShaderStage::Fragment);
        const GeneratedBindGroupLayout *material_layout = nullptr;
        const GeneratedBindGroupLayout *draw_layout_desc = nullptr;
        for (const GeneratedBindGroupLayout &layout : generated) {
            if (layout.set == 0) material_layout = &layout;
            else if (layout.set == 1) draw_layout_desc = &layout;
            else return unexpected(mesh_path_error("displacement mesh shader declares an unexpected descriptor set."));
        }
        if (material_layout == nullptr || draw_layout_desc == nullptr) {
            return unexpected(mesh_path_error("displacement mesh shader reflection lacks a material set (0) or a draw set (1)."));
        }
        usize template_set0 = tmpl.bind_group_layout_sets.size();
        for (usize i = 0; i < tmpl.bind_group_layout_sets.size(); ++i) {
            if (tmpl.bind_group_layout_sets[i] == 0) template_set0 = i;
        }
        if (template_set0 >= tmpl.bind_group_layouts.size() || tmpl.bind_group_layout_sets.size() != 1) {
            return unexpected(mesh_path_error("displaced material template must have exactly one bind group set (0)."));
        }
        // Compare against the layout the template was actually built with. Its entries are not exposed once
        // created, so re-derive it the way build_material_template_gpu does and compare the two derivations.
        {
            const vector<GeneratedBindGroupLayout> vertex_side =
                generate_bind_group_layouts(tmpl.shader.reflection(), RHI::ShaderStage::AllGraphics);
            if (vertex_side.empty() || vertex_side.front().entries.size() != material_layout->entries.size()) {
                return unexpected(mesh_path_error("mesh path material set differs from the vertex path's (entry count); "
                                                  "the two shaders must declare identical material parameters."));
            }
            for (usize i = 0; i < material_layout->entries.size(); ++i) {
                const RHI::BindGroupLayoutEntry &a = vertex_side.front().entries[i];
                const RHI::BindGroupLayoutEntry &b = material_layout->entries[i];
                if (a.binding != b.binding || a.type != b.type || a.count != b.count) {
                    return unexpected(mesh_path_error("mesh path material set differs from the vertex path's at binding " +
                                                      std::to_string(b.binding) + "."));
                }
            }
        }

        if (!resources->draw_layout) {
            vector<RHI::BindGroupLayoutEntry> entries = draw_layout_desc->entries;
            for (RHI::BindGroupLayoutEntry &entry : entries) {
                entry.visibility = kMeshStages;
            }
            auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{entries.data(), entries.size()},
                .label = "displacement mesh draw layout",
            });
            if (!layout) {
                return unexpected(graphics_error_from_rhi(layout.error(), "create displacement mesh draw layout"));
            }
            resources->draw_layout = *layout;
        }

        // ---- Modules --------------------------------------------------------------------------------------
        const auto make_module = [&](const char *entry, const char *label,
                                     RHI::ShaderModuleHandle &out) -> Core::RendererResult {
            auto code = shader.entry_point_code(entry, shader_target->slang_target.format);
            if (!code) {
                return unexpected(mesh_path_error(string{"generate displacement mesh bytecode failed ("} + entry +
                                                  "): " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), label));
            }
            out = *module;
            return {};
        };
        DisplacementMeshTemplateState fresh{};
        fresh.settings = state.settings;
        Core::RendererResult made = make_module("taskMain", "displacement task module", fresh.task_module);
        if (made) made = make_module("meshMain", "displacement mesh module", fresh.mesh_module);
        if (made) made = make_module("fragmentMain", "displacement fragment module", fresh.fragment_module);
        if (made) {
            made = make_module("depthOnlyMain", "displacement depth-only fragment module", fresh.depth_only_fragment_module);
            fresh.has_depth_only_fragment = made.has_value();
        }
        if (!made) {
            for (RHI::ShaderModuleHandle module : {fresh.task_module, fresh.mesh_module, fresh.fragment_module,
                                                    fresh.depth_only_fragment_module}) {
                if (module) device->destroy_shader_module(module);
            }
            return made;
        }

        const array<RHI::BindGroupLayoutHandle, 2> layouts{tmpl.bind_group_layouts[template_set0], resources->draw_layout};
        const vector<RHI::PushConstantRange> push_ranges = generate_push_constant_ranges(reflection, kMeshStages);
        if (push_ranges.size() != 1 || push_ranges.front().size != sizeof(DisplacementMeshGpuDrawConstants)) {
            for (RHI::ShaderModuleHandle module : {fresh.task_module, fresh.mesh_module, fresh.fragment_module,
                                                    fresh.depth_only_fragment_module}) {
                if (module) device->destroy_shader_module(module);
            }
            return unexpected(mesh_path_error("displacement mesh shader's draw push-constant block does not match "
                                              "DisplacementMeshGpuDrawConstants (" +
                                              std::to_string(sizeof(DisplacementMeshGpuDrawConstants)) + " bytes)."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "displacement mesh pipeline layout",
        });
        if (!pipeline_layout) {
            for (RHI::ShaderModuleHandle module : {fresh.task_module, fresh.mesh_module, fresh.fragment_module,
                                                    fresh.depth_only_fragment_module}) {
                if (module) device->destroy_shader_module(module);
            }
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create displacement mesh pipeline layout"));
        }
        fresh.pipeline_layout = *pipeline_layout;
        fresh.built = true;

        shader.release_compiler_state();
        cache.release_compiler_memory();
        state = std::move(fresh);
        Foundation::log_info("Displacement mesh path built for material template '{}' (target edge {} px, max level {}).",
                             tmpl.label, state.settings.target_edge_pixels, state.settings.max_level);
        return {};
    }

    Core::RendererExpected<Renderer::DisplacementMeshDrawSetup> Renderer::displacement_mesh_pipeline_for(
        MaterialTemplateResource &tmpl, span<const RHI::Format> color_formats, RHI::Format depth_format,
        bool depth_only, bool shadow_map, f32 depth_bias, f32 slope_bias, RHI::CullMode cull_mode,
        RHI::FrontFace front_face, RHI::SampleCount samples) {
        ZoneScopedN("Renderer::displacement_mesh_pipeline_for");
        if (Core::RendererResult built = ensure_displacement_mesh_built(tmpl); !built.has_value()) {
            return unexpected(built.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(mesh_path_error("Cannot build a displacement mesh pipeline without an RHI device."));
        }

        auto resources = displacement_mesh_.lock();
        const auto found = resources->templates.find(tmpl.handle.value);
        if (found == resources->templates.end()) {
            return unexpected(mesh_path_error("Displacement mesh path is not enabled for this material template."));
        }
        DisplacementMeshTemplateState &state = found->second;
        const DisplacementMeshDrawSetup setup_template{
            .target_edge_pixels = state.settings.target_edge_pixels,
            .max_level = state.settings.max_level,
        };
        if (!shadow_map) {
            depth_bias = 0.0f;
            slope_bias = 0.0f;
        }
        for (const DisplacementMeshPipelineVariant &variant : state.variants) {
            if (variant.depth_format == depth_format && variant.depth_only == depth_only &&
                variant.depth_bias == depth_bias && variant.slope_bias == slope_bias &&
                variant.cull_mode == cull_mode && variant.front_face == front_face && variant.samples == samples &&
                variant.color_formats.size() == color_formats.size() &&
                std::equal(variant.color_formats.begin(), variant.color_formats.end(), color_formats.begin())) {
                DisplacementMeshDrawSetup setup = setup_template;
                setup.pipeline = variant.pipeline;
                return setup;
            }
        }

        vector<RHI::ColorTargetState> color_targets;
        if (!depth_only) {
            if (color_formats.empty()) {
                return unexpected(mesh_path_error("Cannot build a displacement mesh pipeline without a colour target."));
            }
            for (RHI::Format format : color_formats) {
                color_targets.push_back(RHI::ColorTargetState{.format = format, .blend_enable = false,
                                                              .write_mask = RHI::ColorWriteMask::All});
            }
        }
        // The G-buffer pass draws these with a standard depth test: displaced geometry is not in the z
        // prepass (its silhouette moves), so nothing has laid down an "equal" depth for it.
        const RHI::DepthStencilState depth_stencil{
            .format = depth_format,
            .depth_test_enable = true,
            .depth_write_enable = true,
            .depth_compare = RHI::CompareOp::Less,
        };
        RHI::RenderPipelineDesc desc{
            .layout = state.pipeline_layout,
            .task = RHI::ShaderEntry{.module = state.task_module, .entry_point = "taskMain", .stage = RHI::ShaderStage::Task},
            .mesh = RHI::ShaderEntry{.module = state.mesh_module, .entry_point = "meshMain", .stage = RHI::ShaderStage::Mesh},
            .fragment = depth_only
                            ? (state.has_depth_only_fragment
                                   ? RHI::ShaderEntry{.module = state.depth_only_fragment_module, .entry_point = "depthOnlyMain", .stage = RHI::ShaderStage::Fragment}
                                   : RHI::ShaderEntry{})
                            : RHI::ShaderEntry{.module = state.fragment_module, .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .rasterization = shadow_map
                ? RHI::RasterizationState{.cull_mode = cull_mode, .front_face = front_face,
                                          .depth_bias_constant = depth_bias, .depth_bias_slope_scale = slope_bias}
                : RHI::RasterizationState{.cull_mode = cull_mode, .front_face = front_face},
            .multisample = RHI::MultisampleState{.samples = samples},
            .depth_stencil = depth_stencil,
            .color_targets = span<const RHI::ColorTargetState>{color_targets.data(), color_targets.size()},
            .label = depth_only ? "displacement mesh depth-only pipeline" : "displacement mesh gbuffer pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create displacement mesh pipeline"));
        }
        Foundation::log_info("Displacement mesh path: created {} pipeline (task + mesh{}, {} colour target(s)).",
                             depth_only ? (shadow_map ? "shadow depth-only" : "depth-only") : "G-buffer",
                             depth_only ? "" : " + fragment", color_targets.size());
        state.variants.push_back(DisplacementMeshPipelineVariant{
            .color_formats = vector<RHI::Format>{color_formats.begin(), color_formats.end()},
            .depth_format = depth_format,
            .depth_only = depth_only,
            .depth_bias = depth_bias,
            .slope_bias = slope_bias,
            .cull_mode = cull_mode,
            .front_face = front_face,
            .samples = samples,
            .pipeline = *pipeline,
        });
        DisplacementMeshDrawSetup setup = setup_template;
        setup.pipeline = *pipeline;
        return setup;
    }

    Core::RendererResult Renderer::prepare_displacement_mesh_frame(WindowSurfaceRecord &record, u64 frame_index,
                                                                   FrameSubmission &submission,
                                                                   Core::Extent2D render_extent) {
        ZoneScopedN("Renderer::prepare_displacement_mesh_frame");
        submission.displacement_mesh_frame.reset();
        if (displacement_mesh_template_count_.load(std::memory_order_relaxed) == 0) {
            return {};
        }
        // Only frames that actually contain draws on the mesh path need any of this.
        bool any_mesh_draw = false;
        for (const RenderItem &item : submission.draws) {
            const MaterialInstanceResource *material = material_instance(item.material);
            if (material != nullptr && displacement_mesh_geometry_active(material->material_template)) {
                any_mesh_draw = true;
                break;
            }
        }
        if (!any_mesh_draw) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(mesh_path_error("Cannot prepare displacement mesh draws without an RHI device."));
        }

        const u32 frame_count = capabilities_.max_frames_in_flight;
        if (record.scene_frame_resources.size() != frame_count) {
            return unexpected(mesh_path_error("Scene frame resources must be prepared before displacement mesh draws."));
        }
        SceneFrameGpuResources &resources = record.scene_frame_resources[frame_index % frame_count];
        // One slot per distinct view drawn this frame (main camera + shadow views). Fixed size: bind groups
        // already recorded reference the buffer, so it cannot grow mid-frame.
        constexpr u32 kViewSlots = 256;
        const u32 stride = align_up(static_cast<u32>(sizeof(DisplacementMeshViewGpuConstants)),
                                    static_cast<u32>(std::max<u64>(device->limits().min_uniform_buffer_offset_alignment, 16)));
        if (!resources.displacement_mesh_buffer) {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = static_cast<u64>(kViewSlots) * stride,
                .usage = RHI::BufferUsage::Uniform,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "displacement mesh view constants",
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), "create displacement mesh view buffer"));
            }
            resources.displacement_mesh_buffer = *buffer;
            resources.displacement_mesh_capacity = kViewSlots;
        }

        auto frame = std::make_unique<DisplacementMeshFrame>();
        frame->lod_camera_position = submission.camera.world_position;
        const f32 fov = std::clamp(submission.camera.vertical_fov_radians, 0.01f, 3.1f);
        frame->pixels_per_unit_at_one = static_cast<f32>(render_extent.y) / (2.0f * std::tan(fov * 0.5f));
        frame->constants_buffer = resources.displacement_mesh_buffer;
        frame->constants_stride = stride;
        frame->constants_capacity = resources.displacement_mesh_capacity;
        frame->transient_bind_groups = &submission.transient_bind_groups;
        submission.displacement_mesh_frame = std::move(frame);
        for (RenderItem &item : submission.draws) {
            item.displacement_frame = submission.displacement_mesh_frame.get();
        }
        return {};
    }

    template <typename Encoder>
    Core::RendererResult Renderer::record_displacement_mesh_item(
        Encoder &pass, const RenderItem &item, MaterialTemplateResource &tmpl, MaterialInstanceResource &material,
        span<const RHI::Format> color_formats, RHI::Format depth_format, u64 frame_index,
        const glm::mat4 &view_projection, bool depth_only, RenderItemBindingState &binding_state, bool shadow_map,
        f32 shadow_depth_bias, f32 shadow_slope_bias, RHI::SampleCount samples) {
        ZoneScopedN("Renderer::record_displacement_mesh_item");
        // The z prepass cannot see displaced silhouettes; the G-buffer pass draws these with a normal
        // depth test instead. Shadow maps DO draw them, displaced, through this same path.
        if (depth_only && !shadow_map) {
            return {};
        }
        DisplacementMeshFrame *frame = const_cast<DisplacementMeshFrame *>(item.displacement_frame);
        MeshResource *mesh_resource = mesh(item.mesh);
        if (frame == nullptr || mesh_resource == nullptr || !mesh_resource->gpu_resident || !vertex_arena_.buffer ||
            !index_arena_.buffer) {
            return mesh_resource == nullptr || !mesh_resource->gpu_resident
                       ? unexpected(mesh_path_error("Render item references a mesh that is not GPU-resident."))
                       : Core::RendererResult{};
        }
        if (mesh_resource->index_count < 3) {
            return {}; // the mesh path refines indexed triangle lists only
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(mesh_path_error("Renderer RHI device is unavailable."));
        }

        auto setup = displacement_mesh_pipeline_for(tmpl, depth_only ? span<const RHI::Format>{} : color_formats,
                                                    depth_format, depth_only, shadow_map, shadow_depth_bias,
                                                    shadow_slope_bias, item.cull_mode, item.front_face, samples);
        if (!setup) {
            return unexpected(setup.error());
        }

        if (!(binding_state.pipeline == setup->pipeline)) {
            pass.set_pipeline(setup->pipeline);
            binding_state.pipeline = setup->pipeline;
            // A different pipeline layout (no push constants, extra set 1) disturbs the sets a vertex
            // pipeline left bound; force them to be rebound by whoever draws next.
            binding_state.material = {};
            binding_state.material_frame_slot = ~0u;
            binding_state.bound_object_history_group = {};
        }

        const u32 frame_slot = material.frames.empty() ? 0u : static_cast<u32>(frame_index % material.frames.size());
        if (!material.frames.empty() &&
            (!(binding_state.material == item.material) || binding_state.material_frame_slot != frame_slot)) {
            auto bind_groups = prepare_material_frame(material, frame_slot);
            if (!bind_groups) {
                return unexpected(bind_groups.error());
            }
            for (usize i = 0; i < bind_groups->size() && i < tmpl.bind_group_layout_sets.size(); ++i) {
                pass.set_bind_group(tmpl.bind_group_layout_sets[i], (*bind_groups)[i]);
            }
            binding_state.material = item.material;
            binding_state.material_frame_slot = frame_slot;
        }

        // Set 1: this view's constants (one uniform-buffer slot + bind group per distinct view, cached for the
        // frame) plus the shared geometry arenas as storage buffers.
        RHI::BindGroupHandle group{};
        {
            auto views = frame->views.lock();
            if (views->vertex_arena != vertex_arena_.buffer || views->index_arena != index_arena_.buffer) {
                // The arenas were reallocated: every cached group still references the old buffers.
                views->entries.clear();
                views->vertex_arena = vertex_arena_.buffer;
                views->index_arena = index_arena_.buffer;
            }
            for (const DisplacementMeshFrame::ViewEntry &entry : views->entries) {
                if (entry.view_projection == view_projection) {
                    group = entry.group;
                    break;
                }
            }
            if (!group) {
                const u32 slot = static_cast<u32>(views->entries.size());
                if (slot >= frame->constants_capacity) {
                    if (!frame->overflow_reported.exchange(true)) {
                        Foundation::log_warn("Displacement mesh view constants exhausted ({} views); displaced draws from "
                                             "further views this frame are skipped.", frame->constants_capacity);
                    }
                    return {};
                }
                const Frustum frustum = frustum_from_view_projection(view_projection);
                DisplacementMeshViewGpuConstants constants{};
                constants.view_projection = view_projection;
                constants.camera_position = glm::vec4{frame->lod_camera_position, 1.0f};
                for (usize p = 0; p < 6; ++p) {
                    constants.frustum_planes[p] = frustum.planes[p];
                }
                constants.pixels_per_unit_at_one = frame->pixels_per_unit_at_one;
                const u64 offset = static_cast<u64>(slot) * frame->constants_stride;
                if (auto written = device->write_buffer(
                        frame->constants_buffer, offset,
                        std::as_bytes(span<const DisplacementMeshViewGpuConstants>{&constants, 1}));
                    !written) {
                    return unexpected(graphics_error_from_rhi(written.error(), "write displacement mesh view constants"));
                }
                RHI::BindGroupLayoutHandle layout{};
                {
                    auto resources = displacement_mesh_.lock();
                    layout = resources->draw_layout;
                }
                const array<RHI::BindGroupEntry, 3> entries{
                    RHI::BindGroupEntry{.binding = 0, .buffer = frame->constants_buffer, .offset = offset,
                                        .size = sizeof(DisplacementMeshViewGpuConstants)},
                    RHI::BindGroupEntry{.binding = 1, .buffer = vertex_arena_.buffer},
                    RHI::BindGroupEntry{.binding = 2, .buffer = index_arena_.buffer},
                };
                auto created = device->create_bind_group(RHI::BindGroupDesc{
                    .layout = layout,
                    .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                    .lifetime = RHI::BindGroupLifetime::FrameTransient,
                    .label = "displacement mesh view bind group",
                });
                if (!created) {
                    return unexpected(graphics_error_from_rhi(created.error(), "create displacement mesh view bind group"));
                }
                if (frame->transient_bind_groups != nullptr) {
                    auto guard = transient_bind_groups_lock_.lock();
                    frame->transient_bind_groups->push_back(*created);
                }
                views->entries.push_back(DisplacementMeshFrame::ViewEntry{.view_projection = view_projection, .group = *created});
                group = *created;
            }
        }
        // Always rebound: a vertex-path draw in between may have put the object-history group in set 1.
        pass.set_bind_group(1, group);

        const f32 scale = std::max({glm::length(glm::vec3{item.world_transform[0]}),
                                    glm::length(glm::vec3{item.world_transform[1]}),
                                    glm::length(glm::vec3{item.world_transform[2]})});

        const u32 total_triangles = mesh_resource->index_count / 3u;
        const u32 triangles_per_draw = kMaxGroupsPerDraw * Displacement::kTaskTrianglesPerGroup;
        for (u32 first = 0; first < total_triangles; first += triangles_per_draw) {
            const u32 triangles = std::min(triangles_per_draw, total_triangles - first);
            DisplacementMeshGpuDrawConstants constants{};
            constants.model = item.world_transform;
            constants.first_index = mesh_resource->index_offset + first * 3u;
            constants.vertex_base = mesh_resource->vertex_offset;
            constants.triangle_count = triangles;
            constants.bounds_scale = scale;
            constants.target_edge_pixels = setup->target_edge_pixels;
            constants.max_level = setup->max_level;
            pass.set_push_constants(kMeshStages, 0,
                                    std::as_bytes(span<const DisplacementMeshGpuDrawConstants>{&constants, 1}));
            pass.draw_mesh_tasks(RHI::DrawMeshTasksArgs{
                .group_count_x = (triangles + Displacement::kTaskTrianglesPerGroup - 1u) /
                                 Displacement::kTaskTrianglesPerGroup,
                .group_count_y = 1,
                .group_count_z = 1,
            });
        }
        return {};
    }

    template Core::RendererResult Renderer::record_displacement_mesh_item<RHI::RenderPassEncoder>(
        RHI::RenderPassEncoder &, const RenderItem &, MaterialTemplateResource &, MaterialInstanceResource &,
        span<const RHI::Format>, RHI::Format, u64, const glm::mat4 &, bool, RenderItemBindingState &, bool, f32, f32,
        RHI::SampleCount);
    template Core::RendererResult Renderer::record_displacement_mesh_item<RHI::RenderBundleEncoder>(
        RHI::RenderBundleEncoder &, const RenderItem &, MaterialTemplateResource &, MaterialInstanceResource &,
        span<const RHI::Format>, RHI::Format, u64, const glm::mat4 &, bool, RenderItemBindingState &, bool, f32, f32,
        RHI::SampleCount);

} // namespace SFT::Renderer
