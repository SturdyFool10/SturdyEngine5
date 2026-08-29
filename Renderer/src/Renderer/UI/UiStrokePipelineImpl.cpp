#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#pragma endregion

#include <Renderer/UI/UiStrokePipeline.hpp>

#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>

using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::UI {

    /// Destroys the UI stroke frame resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_ui_stroke_frame_resources(RHI::RhiDevice &device, UiStrokeFrameResources &resources) noexcept {
        for (const UiStrokeDrawBatch::BoundGroup &group : resources.bind_groups) {
            if (group.handle) {
                device.destroy_bind_group(group.handle);
            }
        }
        if (resources.instance_buffer) {
            device.destroy_buffer(resources.instance_buffer);
        }
        resources = {};
    }

    namespace {
        namespace slang = Core::Slang;

        /// Binds group layout index for set for subsequent operations.
        ///
        /// @param sets `sets` value used by the operation.
        /// @param set `set` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize bind_group_layout_index_for_set(span<const u32> sets, u32 set) noexcept {
            for (usize i = 0; i < sets.size(); ++i) {
                if (sets[i] == set) {
                    return i;
                }
            }
            return sets.size();
        }

        /// Performs the same instance operation for `UI` using the supplied arguments.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool same_instance(const UiStrokeInstance &lhs, const UiStrokeInstance &rhs) noexcept {
            return std::memcmp(&lhs, &rhs, sizeof(UiStrokeInstance)) == 0;
        }

        /// Performs the same rect operation for `UI` using the supplied arguments.
        ///
        /// @param lhs Left-hand operand.
        /// @param rhs Right-hand operand.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool same_rect(const RHI::Rect2D &lhs, const RHI::Rect2D &rhs) noexcept {
            return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
        }


        struct UiViewConstantsGpu {
            glm::vec2 viewport_size{0.0f};
            u32 instance_index_base = 0;
            u32 padding = 0;
        };

        /// Creates an error result describing the supplied UI stroke failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError ui_stroke_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

    } // namespace

    /// Creates a `UI` resource or value from the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param color_format Format used for the resource, render target, or conversion.
    /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<UiStrokePipeline> UiStrokePipeline::create(
        RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache) {
        const auto shader_target = Renderer::shader_target_for_device(device);
        if (!shader_target) {
            return unexpected(shader_target.error());
        }
        const slang::ShaderCompileOptions options{
            .targets = Renderer::shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/ui_stroke.slang", "ui_stroke"),
            options,
            slang::ShaderCompiler{},
            enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(ui_stroke_error("compile ui_stroke shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }

        UiStrokePipeline pipeline;

        auto vertex_code = shader->entry_point_code("vertexMain", shader_target->slang_target.format);
        if (!vertex_code) {
            return unexpected(ui_stroke_error("generate ui_stroke vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "ui stroke vertex module",
        });
        if (!vertex_module) {
            return unexpected(Renderer::graphics_error_from_rhi(vertex_module.error(), "create ui stroke vertex module"));
        }
        pipeline.vertex_module_ = *vertex_module;

        auto fragment_code = shader->entry_point_code("fragmentMain", shader_target->slang_target.format);
        if (!fragment_code) {
            pipeline.destroy(device);
            return unexpected(ui_stroke_error("generate ui_stroke fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "ui stroke fragment module",
        });
        if (!fragment_module) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(fragment_module.error(), "create ui stroke fragment module"));
        }
        pipeline.fragment_module_ = *fragment_module;

        const slang::ShaderReflection &reflection = shader->reflection();
        const RHI::ShaderStage stage_mask = Renderer::reflected_stage_mask(reflection);
        const vector<Renderer::GeneratedBindGroupLayout> generated = Renderer::generate_bind_group_layouts(reflection, stage_mask);
        if (generated.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_stroke_error("ui_stroke shader produced no bind-group layout."));
        }
        for (const Renderer::GeneratedBindGroupLayout &layout : generated) {
            auto handle = device.create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "ui stroke bind group layout",
            });
            if (!handle) {
                pipeline.destroy(device);
                return unexpected(Renderer::graphics_error_from_rhi(handle.error(), "create ui stroke bind group layout"));
            }
            pipeline.bind_group_layouts_.push_back(*handle);
            pipeline.bind_group_layout_sets_.push_back(layout.set);
        }

        const vector<Renderer::ReflectedResource> resources = Renderer::collect_resource_bindings(reflection);
        auto resolve = [&](string_view name) -> ResourceBinding {
            for (const Renderer::ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    const usize index = bind_group_layout_index_for_set(pipeline.bind_group_layout_sets_, resource.set);
                    if (index < pipeline.bind_group_layouts_.size()) {
                        return ResourceBinding{.layout_index = index, .binding = resource.binding, .found = true};
                    }
                }
            }
            return ResourceBinding{};
        };
        pipeline.instances_binding_ = resolve("instances");
        if (!pipeline.instances_binding_.found) {
            pipeline.destroy(device);
            return unexpected(ui_stroke_error("ui_stroke shader reflection did not produce the expected instances binding."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges = Renderer::generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex);
        if (push_constant_ranges.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_stroke_error("ui_stroke shader reflection did not produce the expected viewConstants push-constant range."));
        }
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{pipeline.bind_group_layouts_.data(), pipeline.bind_group_layouts_.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "ui stroke pipeline layout",
        });
        if (!pipeline_layout) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(pipeline_layout.error(), "create ui stroke pipeline layout"));
        }
        pipeline.pipeline_layout_ = *pipeline_layout;

        const RHI::ColorTargetState color_target{
            .format = color_format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::SrcAlpha, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::OneMinusSrcAlpha, .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        const RHI::RenderPipelineDesc desc{
            .layout = pipeline.pipeline_layout_,
            .vertex = RHI::ShaderEntry{.module = pipeline.vertex_module_, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = pipeline.fragment_module_, .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "ui stroke pipeline",
        };
        auto rhi_pipeline = device.create_render_pipeline(desc);
        if (!rhi_pipeline) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(rhi_pipeline.error(), "create ui stroke pipeline"));
        }
        pipeline.pipeline_ = *rhi_pipeline;

        return pipeline;
    }

    /// Prepares the required state or resources for a later operation.
    ///
    /// @param device Device used or affected by the operation.
    /// @param instances Instance used or affected by the operation.
    /// @param instance_scissors Instance used or affected by the operation.
    /// @param instance_paint_groups Instance used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    /// @param out_batches `out_batches` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult UiStrokePipeline::prepare(RHI::RhiDevice &device, span<const UiStrokeInstance> instances,
                                                   span<const RHI::Rect2D> instance_scissors,
                                                   span<const u32> instance_paint_groups,
                                                   UiStrokeFrameResources &resources, vector<UiStrokeDrawBatch> &out_batches) {
        out_batches.clear();
        if (instances.size() != instance_scissors.size() || instances.size() != instance_paint_groups.size()) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "UiStrokePipeline::prepare: instance/scissor/paint-group counts must match.");
        }
        if (instances.empty()) {
            return {};
        }

        usize i = 0;
        while (i < instances.size()) {
            usize j = i + 1;
            while (j < instances.size() && same_rect(instance_scissors[j], instance_scissors[i]) &&
                   instance_paint_groups[j] == instance_paint_groups[i]) {
                ++j;
            }
            out_batches.push_back(UiStrokeDrawBatch{
                .scissor = instance_scissors[i],
                .first_instance = static_cast<u32>(i),
                .instance_count = static_cast<u32>(j - i),
                .paint_group = instance_paint_groups[i],
            });
            i = j;
        }

        const u64 required_bytes = static_cast<u64>(instances.size()) * sizeof(UiStrokeInstance);
        bool buffer_replaced = false;
        if (!resources.instance_buffer || resources.instance_capacity_bytes < required_bytes) {
            u64 new_capacity = std::max<u64>(sizeof(UiStrokeInstance) * 64u, resources.instance_capacity_bytes);
            while (new_capacity < required_bytes) {
                new_capacity = new_capacity > std::numeric_limits<u64>::max() / 2u ? required_bytes : new_capacity * 2u;
            }
            auto replacement = device.create_buffer(RHI::BufferDesc{
                .size = new_capacity,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "persistent ui stroke instance buffer",
            });
            if (!replacement) {
                out_batches.clear();
                return unexpected(Renderer::graphics_error_from_rhi(replacement.error(), "create persistent ui stroke instance buffer"));
            }
            destroy_ui_stroke_frame_resources(device, resources);
            resources.instance_buffer = *replacement;
            resources.instance_capacity_bytes = new_capacity;
            buffer_replaced = true;
        }

        const bool instances_unchanged = resources.uploaded_instances.size() == instances.size() &&
            std::ranges::equal(resources.uploaded_instances, instances, same_instance);
        if (!instances_unchanged) {
            const auto *bytes = reinterpret_cast<const std::byte *>(instances.data());
            if (auto written = device.write_buffer(resources.instance_buffer, 0,
                                                   span<const std::byte>{bytes, static_cast<usize>(required_bytes)});
                !written) {
                out_batches.clear();
                return unexpected(Renderer::graphics_error_from_rhi(written.error(), "write ui stroke instance buffer"));
            }
            resources.uploaded_instances.assign(instances.begin(), instances.end());
        }

        if (buffer_replaced || !resources.bind_groups_valid) {
            for (const UiStrokeDrawBatch::BoundGroup &group : resources.bind_groups) {
                if (group.handle) {
                    device.destroy_bind_group(group.handle);
                }
            }
            resources.bind_groups.clear();

            struct Group {
                usize layout_index = 0;
                vector<RHI::BindGroupEntry> entries;
            };
            vector<Group> groups;
            auto add_entry = [&](const ResourceBinding &binding, RHI::BindGroupEntry value) {
                value.binding = binding.binding;
                auto group = std::ranges::find(groups, binding.layout_index, &Group::layout_index);
                if (group == groups.end()) {
                    groups.push_back(Group{.layout_index = binding.layout_index});
                    group = std::prev(groups.end());
                }
                group->entries.push_back(value);
            };
            add_entry(instances_binding_, RHI::BindGroupEntry{
                                              .buffer = resources.instance_buffer,
                                              .offset = 0,
                                              .size = 0,
                                              .structure_stride = sizeof(UiStrokeInstance),
                                          });
            for (const Group &group : groups) {
                auto bind_group = device.create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layouts_[group.layout_index],
                    .entries = span<const RHI::BindGroupEntry>{group.entries.data(), group.entries.size()},
                    .label = "persistent ui stroke bind group",
                });
                if (!bind_group) {
                    for (const UiStrokeDrawBatch::BoundGroup &created : resources.bind_groups) {
                        device.destroy_bind_group(created.handle);
                    }
                    resources.bind_groups.clear();
                    out_batches.clear();
                    return unexpected(Renderer::graphics_error_from_rhi(bind_group.error(), "create persistent ui stroke bind group"));
                }
                resources.bind_groups.push_back(UiStrokeDrawBatch::BoundGroup{
                    .set = bind_group_layout_sets_[group.layout_index],
                    .handle = *bind_group,
                });
            }
            resources.bind_groups_valid = true;
        }

        for (UiStrokeDrawBatch &batch : out_batches) {
            batch.instance_buffer = resources.instance_buffer;
            batch.bind_groups = resources.bind_groups;
        }
        return {};
    }

    /// Draws the requested content using the current rendering state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param batches `batches` value used by the operation.
    /// @param viewport_size Requested or available size for the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult UiStrokePipeline::draw(RHI::RenderPassEncoder &pass, span<const UiStrokeDrawBatch> batches,
                                                glm::vec2 viewport_size) {
        if (batches.empty()) {
            return {};
        }
        if (!pipeline_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "UI stroke pipeline was not created.");
        }

        pass.set_pipeline(pipeline_);

        for (const UiStrokeDrawBatch &batch : batches) {
            if (!batch.instance_buffer) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI stroke draw batch has no prepared instance buffer.");
            }
            pass.set_scissor(batch.scissor);
            for (const UiStrokeDrawBatch::BoundGroup &group : batch.bind_groups) {
                if (!group.handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "UI stroke draw batch has an invalid persistent bind group.");
                }
                pass.set_bind_group(group.set, group.handle);
            }

            const UiViewConstantsGpu constants{
                .viewport_size = viewport_size,
                .instance_index_base = batch.first_instance,
            };
            pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                    span<const std::byte>{reinterpret_cast<const std::byte *>(&constants), sizeof(constants)});

            pass.draw(RHI::DrawArgs{
                .vertex_count = 6,
                .instance_count = batch.instance_count,
                .first_vertex = 0,
                .first_instance = 0,
            });
        }
        return {};
    }

    /// Destroys or releases the `UI` resource represented by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiStrokePipeline::destroy(RHI::RhiDevice &device) noexcept {
        if (pipeline_) {
            device.destroy_render_pipeline(pipeline_);
        }
        if (pipeline_layout_) {
            device.destroy_pipeline_layout(pipeline_layout_);
        }
        for (RHI::BindGroupLayoutHandle layout : bind_group_layouts_) {
            device.destroy_bind_group_layout(layout);
        }
        if (fragment_module_) {
            device.destroy_shader_module(fragment_module_);
        }
        if (vertex_module_) {
            device.destroy_shader_module(vertex_module_);
        }
        *this = UiStrokePipeline{};
    }

} // namespace SFT::UI
