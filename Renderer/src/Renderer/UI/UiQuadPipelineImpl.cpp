#include <atomic>
#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <glm/vec3.hpp>
#include <limits>
#include <string>
#include <string_view>
#pragma endregion

#include <Renderer/UI/UiQuadPipeline.hpp>

#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/ShaderTarget.hpp>
#include <Renderer/TileGrid.hpp>

using std::string;
using std::string_view;
using std::unexpected;

namespace SFT::UI {

    /// Destroys the UI quad frame resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_ui_quad_frame_resources(RHI::RhiDevice &device, UiQuadFrameResources &resources) noexcept {
        for (UiQuadFrameResources::BindingCacheEntry &entry : resources.binding_cache) {
            for (u32 i = 0; i < entry.bind_group_count; ++i) {
                if (entry.bind_groups[i].handle) {
                    device.destroy_bind_group(entry.bind_groups[i].handle);
                }
            }
        }
        for (u32 i = 0; i < resources.table_group_count; ++i) {
            if (resources.table_groups[i].handle) {
                device.destroy_bind_group(resources.table_groups[i].handle);
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
        [[nodiscard]] bool same_instance(const UiQuadInstanceGpu &lhs, const UiQuadInstanceGpu &rhs) noexcept {
            return std::memcmp(&lhs, &rhs, sizeof(UiQuadInstanceGpu)) == 0;
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

        /// Creates an error result describing the supplied UI quad failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError ui_quad_error(string message) {
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
    Core::RendererExpected<UiQuadPipeline> UiQuadPipeline::create(
        RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache) {
        // Textures as one array when the device can index descriptors; if that path cannot be built for any reason
        // the pipeline still comes up the one-texture-per-bind-group way.
        const RHI::FeatureSet bindless_features = RHI::features_of({
            RHI::Feature::BindlessResources,
            RHI::Feature::RuntimeDescriptorArrays,
            RHI::Feature::DescriptorBindingVariableCount,
            RHI::Feature::DescriptorBindingPartiallyBound,
            RHI::Feature::DescriptorBindingUpdateAfterBind,
            RHI::Feature::SampledImageArrayNonUniformIndexing,
        });
        const u32 variable_limit = device.feature_properties().descriptor_indexing.max_variable_descriptor_count;
        // Vulkan uses the table by default (STURDY_UI_BINDLESS=0 turns it off, to compare the two or to rule it out). On
        // D3D12 it has not been exercised on a real device yet, so it stays off there unless STURDY_UI_BINDLESS=1 asks.
        const char *forced = std::getenv("STURDY_UI_BINDLESS");
        const std::string_view forced_value = forced != nullptr ? std::string_view{forced} : std::string_view{};
        const bool allowed = forced_value == "0" ? false
                             : forced_value == "1" ? true
                                                   : device.backend_type() == RHI::BackendType::Vulkan;
        if (allowed && device.enabled_features().contains_all(bindless_features) && variable_limit >= kMinBindlessCapacity) {
            auto bindless = create_variant(device, color_format, enable_shader_disk_cache, std::min(kBindlessCapacity, variable_limit));
            if (bindless) {
                return bindless;
            }
            Foundation::log_warn("UI: the bindless quad pipeline could not be built ({}); falling back to one bind group per texture.",
                                 bindless.error().message);
        }
        return create_variant(device, color_format, enable_shader_disk_cache, 0);
    }

    Core::RendererExpected<UiQuadPipeline> UiQuadPipeline::create_variant(
        RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache, u32 bindless_capacity) {
        const auto shader_target = Renderer::shader_target_for_device(device);
        if (!shader_target) {
            return unexpected(shader_target.error());
        }
        slang::ShaderCompileOptions options{
            .targets = Renderer::shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        if (bindless_capacity != 0) {
            options.macros.push_back(slang::ShaderMacro{.name = "SFT_UI_BINDLESS", .value = "1"});
        }
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/ui_quad.slang", "ui_quad"),
            options,
            slang::ShaderCompiler{},
            enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(ui_quad_error("compile ui_quad shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }

        UiQuadPipeline pipeline;
        pipeline.bindless_ = bindless_capacity != 0;
        pipeline.bindless_capacity_ = bindless_capacity;

        auto vertex_code = shader->entry_point_code("vertexMain", shader_target->slang_target.format);
        if (!vertex_code) {
            return unexpected(ui_quad_error("generate ui_quad vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "ui quad vertex module",
        });
        if (!vertex_module) {
            return unexpected(Renderer::graphics_error_from_rhi(vertex_module.error(), "create ui quad vertex module"));
        }
        pipeline.vertex_module_ = *vertex_module;

        auto fragment_code = shader->entry_point_code("fragmentMain", shader_target->slang_target.format);
        if (!fragment_code) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("generate ui_quad fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device.create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "ui quad fragment module",
        });
        if (!fragment_module) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(fragment_module.error(), "create ui quad fragment module"));
        }
        pipeline.fragment_module_ = *fragment_module;

        const slang::ShaderReflection &reflection = shader->reflection();
        const RHI::ShaderStage stage_mask = Renderer::reflected_stage_mask(reflection);
        const vector<Renderer::GeneratedBindGroupLayout> generated = Renderer::generate_bind_group_layouts(
            reflection, stage_mask, bindless_capacity != 0 ? bindless_capacity : 4096u);
        if (generated.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("ui_quad shader produced no bind-group layout."));
        }
        if (generated.size() > kUiQuadMaxBindGroups) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("ui_quad shader produced more bind-group layouts than a quad batch can hold."));
        }
        for (const Renderer::GeneratedBindGroupLayout &layout : generated) {
            auto handle = device.create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "ui quad bind group layout",
            });
            if (!handle) {
                pipeline.destroy(device);
                return unexpected(Renderer::graphics_error_from_rhi(handle.error(), "create ui quad bind group layout"));
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
        pipeline.texture_binding_ = resolve(pipeline.bindless_ ? "quadTextures" : "quadTexture");
        pipeline.sampler_binding_ = resolve("quadSampler");
        pipeline.nearest_sampler_binding_ = resolve("quadSamplerNearest");
        if (!pipeline.instances_binding_.found || !pipeline.texture_binding_.found || !pipeline.sampler_binding_.found) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error(
                "ui_quad shader reflection did not produce the expected instances/texture/quadSampler bindings."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges = Renderer::generate_push_constant_ranges(reflection, RHI::ShaderStage::Vertex);
        if (push_constant_ranges.empty()) {
            pipeline.destroy(device);
            return unexpected(ui_quad_error("ui_quad shader reflection did not produce the expected viewConstants push-constant range."));
        }
        auto pipeline_layout = device.create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{pipeline.bind_group_layouts_.data(), pipeline.bind_group_layouts_.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "ui quad pipeline layout",
        });
        if (!pipeline_layout) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(pipeline_layout.error(), "create ui quad pipeline layout"));
        }
        pipeline.pipeline_layout_ = *pipeline_layout;

        auto sampler = device.create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "ui quad sampler",
        });
        if (!sampler) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(sampler.error(), "create ui quad sampler"));
        }
        pipeline.sampler_ = *sampler;

        auto nearest_sampler = device.create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Nearest,
            .mag_filter = RHI::Filter::Nearest,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "ui quad nearest sampler",
        });
        if (!nearest_sampler) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(nearest_sampler.error(), "create ui quad nearest sampler"));
        }
        pipeline.nearest_sampler_ = *nearest_sampler;

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
            .label = "ui quad pipeline",
        };
        auto rhi_pipeline = device.create_render_pipeline(desc);
        if (!rhi_pipeline) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(rhi_pipeline.error(), "create ui quad pipeline"));
        }
        pipeline.pipeline_ = *rhi_pipeline;

        // Same shaders/layout/geometry as the normal pipeline above — only the blend state differs.
        // Additive (One,One) adds light on top of whatever this render pass has already drawn instead
        // of alpha-compositing over it, which a bloom composite needs and ordinary UI quads don't; dst
        // alpha is left untouched (src_factor = Zero) since this draws inside an already-open pass and
        // shouldn't perturb the surface's own coverage/alpha.
        const RHI::ColorTargetState additive_color_target{
            .format = color_format,
            .blend_enable = true,
            .color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::One, .op = RHI::BlendOp::Add},
            .alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::Zero, .dst_factor = RHI::BlendFactor::One, .op = RHI::BlendOp::Add},
            .write_mask = RHI::ColorWriteMask::All,
        };
        const RHI::RenderPipelineDesc additive_desc{
            .layout = pipeline.pipeline_layout_,
            .vertex = RHI::ShaderEntry{.module = pipeline.vertex_module_, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = pipeline.fragment_module_, .entry_point = "fragmentMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&additive_color_target, 1},
            .label = "ui quad additive pipeline",
        };
        auto rhi_additive_pipeline = device.create_render_pipeline(additive_desc);
        if (!rhi_additive_pipeline) {
            pipeline.destroy(device);
            return unexpected(Renderer::graphics_error_from_rhi(rhi_additive_pipeline.error(), "create ui quad additive pipeline"));
        }
        pipeline.additive_pipeline_ = *rhi_additive_pipeline;

        return pipeline;
    }

    /// Prepares the required state or resources for a later operation.
    ///
    /// @param device Device used or affected by the operation.
    /// @param instances Instance used or affected by the operation.
    /// @param instance_texture_views Texture used or affected by the operation.
    /// @param instance_scissors Instance used or affected by the operation.
    /// @param instance_paint_groups Instance used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    /// @param out_batches `out_batches` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult UiQuadPipeline::prepare(RHI::RhiDevice &device, span<const UiQuadInstance> instances,
                                                 span<const RHI::TextureViewHandle> instance_texture_views,
                                                 span<const u64> instance_texture_revisions,
                                                 span<const RHI::Rect2D> instance_scissors,
                                                 span<const u32> instance_paint_groups,
                                                 UiQuadFrameResources &resources, vector<UiQuadDrawBatch> &out_batches) {
        out_batches.clear();
        resources.stats = {};
        if (instances.size() != instance_texture_views.size() || instances.size() != instance_texture_revisions.size() ||
            instances.size() != instance_scissors.size() ||
            instances.size() != instance_paint_groups.size()) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "UiQuadPipeline::prepare: instance/texture-view/scissor/paint-group counts must match.");
        }
        if (instances.empty()) {
            return {};
        }

        // A quad whose texture is gone (an adopted texture created on a device that has since been replaced, or one
        // the app destroyed) draws nothing; it must not fail the whole frame, UI included.
        const usize drawable = static_cast<usize>(std::ranges::count_if(
            instance_texture_views, [](RHI::TextureViewHandle view) { return static_cast<bool>(view); }));
        if (const usize dropped = instances.size() - drawable; dropped != 0) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                Foundation::log_warn("UI: skipped {} quad(s) referencing an invalid texture view (once per run).", dropped);
            }
        }
        if (drawable == 0) {
            return {};
        }

        const u64 required_bytes = static_cast<u64>(drawable) * sizeof(UiQuadInstanceGpu);
        if (!resources.instance_buffer || resources.instance_capacity_bytes < required_bytes) {
            u64 new_capacity = std::max<u64>(sizeof(UiQuadInstanceGpu) * 64u, resources.instance_capacity_bytes);
            while (new_capacity < required_bytes) {
                new_capacity = new_capacity > std::numeric_limits<u64>::max() / 2u ? required_bytes : new_capacity * 2u;
            }
            auto replacement = device.create_buffer(RHI::BufferDesc{
                .size = new_capacity,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "persistent ui quad instance buffer",
            });
            if (!replacement) {
                return unexpected(Renderer::graphics_error_from_rhi(replacement.error(), "create persistent ui quad instance buffer"));
            }
            // Every bind group of the old buffer goes with it, and the table starts over.
            destroy_ui_quad_frame_resources(device, resources);
            resources.instance_buffer = *replacement;
            resources.instance_capacity_bytes = new_capacity;
        }

        // Builds the bind groups of one layout set each from `entries` (each entry tagged with the layout it belongs to).
        struct LayoutEntry {
            usize layout_index = 0;
            RHI::BindGroupEntry entry{};
        };
        const auto build_groups = [&](span<const LayoutEntry> entries, u32 variable_count, const char *label,
                                      std::array<UiQuadDrawBatch::BoundGroup, kUiQuadMaxBindGroups> &out_groups,
                                      u32 &out_count) -> Core::RendererResult {
            out_count = 0;
            for (usize layout_index = 0; layout_index < bind_group_layouts_.size(); ++layout_index) {
                vector<RHI::BindGroupEntry> group_entries;
                for (const LayoutEntry &candidate : entries) {
                    if (candidate.layout_index == layout_index) {
                        group_entries.push_back(candidate.entry);
                    }
                }
                if (group_entries.empty()) {
                    continue;
                }
                auto bind_group = device.create_bind_group(RHI::BindGroupDesc{
                    .layout = bind_group_layouts_[layout_index],
                    .entries = span<const RHI::BindGroupEntry>{group_entries.data(), group_entries.size()},
                    .variable_descriptor_count = variable_count,
                    .label = label,
                });
                if (!bind_group) {
                    for (u32 i = 0; i < out_count; ++i) {
                        device.destroy_bind_group(out_groups[i].handle);
                    }
                    out_count = 0;
                    return unexpected(Renderer::graphics_error_from_rhi(bind_group.error(), label));
                }
                out_groups[out_count++] = UiQuadDrawBatch::BoundGroup{
                    .set = bind_group_layout_sets_[layout_index],
                    .handle = *bind_group,
                };
                ++resources.stats.bind_groups_created;
            }
            return {};
        };
        const auto shared_entries = [&](vector<LayoutEntry> &entries) {
            entries.push_back(LayoutEntry{instances_binding_.layout_index,
                                          RHI::BindGroupEntry{.binding = instances_binding_.binding,
                                                              .buffer = resources.instance_buffer,
                                                              .offset = 0,
                                                              .size = 0,
                                                              .structure_stride = sizeof(UiQuadInstanceGpu)}});
            entries.push_back(LayoutEntry{sampler_binding_.layout_index,
                                          RHI::BindGroupEntry{.binding = sampler_binding_.binding, .sampler = sampler_}});
            if (nearest_sampler_binding_.found) {
                entries.push_back(LayoutEntry{nearest_sampler_binding_.layout_index,
                                              RHI::BindGroupEntry{.binding = nearest_sampler_binding_.binding,
                                                                  .sampler = nearest_sampler_}});
            }
        };

        // Bindless: one table of every texture the frame draws, and one bind group for the whole frame. The table keeps
        // its order and its bind group for as long as the set of textures stays the same; a texture that appears or
        // goes away rebuilds it once.
        if (bindless_) {
            auto &table = resources.table;
            auto &used = resources.table_used;
            used.assign(table.size(), 0u);
            bool rebuild = resources.table_group_count == 0;
            for (usize i = 0; i < instances.size(); ++i) {
                const RHI::TextureViewHandle view = instance_texture_views[i];
                if (!view) {
                    continue;
                }
                const auto found = resources.table_index.find(view.value);
                if (found != resources.table_index.end() && table[found->second].revision == instance_texture_revisions[i]) {
                    used[found->second] = 1u;
                } else {
                    rebuild = true;
                }
            }
            if (!rebuild && std::ranges::find(used, 0u) != used.end()) {
                rebuild = true;
            }
            if (rebuild) {
                vector<UiQuadFrameResources::TableEntry> &next = resources.table_wanted;
                next.clear();
                resources.table_index.clear();
                for (usize i = 0; i < table.size(); ++i) {
                    if (used[i] != 0u) {
                        resources.table_index.emplace(table[i].view.value, static_cast<u32>(next.size()));
                        next.push_back(table[i]);
                    }
                }
                for (usize i = 0; i < instances.size(); ++i) {
                    const RHI::TextureViewHandle view = instance_texture_views[i];
                    if (!view || resources.table_index.contains(view.value)) {
                        continue;
                    }
                    if (next.size() >= bindless_capacity_) {
                        static std::atomic<bool> warned{false};
                        if (!warned.exchange(true)) {
                            Foundation::log_warn("UI: a frame draws more than {} different textures; the rest draw with the first one (once per run).",
                                                 bindless_capacity_);
                        }
                        continue;
                    }
                    resources.table_index.emplace(view.value, static_cast<u32>(next.size()));
                    next.push_back(UiQuadFrameResources::TableEntry{.view = view, .revision = instance_texture_revisions[i]});
                }
                table.swap(next);
                for (u32 i = 0; i < resources.table_group_count; ++i) {
                    if (resources.table_groups[i].handle) {
                        device.destroy_bind_group(resources.table_groups[i].handle);
                    }
                }
                resources.table_group_count = 0;

                vector<LayoutEntry> entries;
                entries.reserve(table.size() + 3);
                shared_entries(entries);
                for (usize i = 0; i < table.size(); ++i) {
                    entries.push_back(LayoutEntry{texture_binding_.layout_index,
                                                  RHI::BindGroupEntry{.binding = texture_binding_.binding,
                                                                      .array_element = static_cast<u32>(i),
                                                                      .texture_view = table[i].view}});
                }
                if (Core::RendererResult built = build_groups(entries, static_cast<u32>(table.size()),
                                                              "ui quad bindless bind group", resources.table_groups,
                                                              resources.table_group_count);
                    !built) {
                    table.clear();
                    resources.table_index.clear();
                    return built;
                }
            }
            resources.stats.textures = static_cast<u32>(table.size());
        }

        // The records themselves, packed straight into the vector that becomes the upload, and the batches: runs of
        // quads that share a scissor and a paint group (and, without bindless, a texture).
        vector<UiQuadInstanceGpu> &staged = resources.staged_instances;
        staged.clear();
        staged.reserve(drawable);
        for (usize i = 0; i < instances.size(); ++i) {
            const RHI::TextureViewHandle view = instance_texture_views[i];
            if (!view) {
                continue;
            }
            u32 texture_index = 0;
            if (bindless_) {
                if (const auto found = resources.table_index.find(view.value); found != resources.table_index.end()) {
                    texture_index = found->second;
                }
            }
            const bool extends = !out_batches.empty() &&
                same_rect(out_batches.back().scissor, instance_scissors[i]) &&
                out_batches.back().paint_group == instance_paint_groups[i] &&
                (bindless_ || (out_batches.back().texture_view == view &&
                               out_batches.back().texture_revision == instance_texture_revisions[i]));
            if (extends) {
                ++out_batches.back().instance_count;
            } else {
                out_batches.push_back(UiQuadDrawBatch{
                    .texture_view = bindless_ ? RHI::TextureViewHandle{} : view,
                    .texture_revision = bindless_ ? 0u : instance_texture_revisions[i],
                    .scissor = instance_scissors[i],
                    .first_instance = static_cast<u32>(staged.size()),
                    .instance_count = 1,
                    .paint_group = instance_paint_groups[i],
                });
            }
            staged.push_back(pack_quad_instance(instances[i], texture_index));
        }
        resources.stats.instances = static_cast<u32>(staged.size());
        resources.stats.batches = static_cast<u32>(out_batches.size());

        const bool instances_unchanged = resources.uploaded_instances.size() == staged.size() &&
            std::ranges::equal(resources.uploaded_instances, staged, same_instance);
        if (!instances_unchanged) {
            const u64 bytes_to_write = static_cast<u64>(staged.size()) * sizeof(UiQuadInstanceGpu);
            const auto *bytes = reinterpret_cast<const std::byte *>(staged.data());
            if (auto written = device.write_buffer(resources.instance_buffer, 0,
                                                   span<const std::byte>{bytes, static_cast<usize>(bytes_to_write)});
                !written) {
                out_batches.clear();
                return unexpected(Renderer::graphics_error_from_rhi(written.error(), "write ui quad instance buffer"));
            }
            resources.uploaded_instances.swap(staged);
            resources.stats.bytes_uploaded = bytes_to_write;
        }

        if (bindless_) {
            for (UiQuadDrawBatch &batch : out_batches) {
                batch.instance_buffer = resources.instance_buffer;
                batch.bind_groups = resources.table_groups;
                batch.bind_group_count = resources.table_group_count;
            }
            return {};
        }

        // One texture per bind group. The cache is keyed by view handle, and a handle value can come back for a
        // different texture once the old one is destroyed (an app reloading an icon or avatar, a rebuilt SVG cache). A
        // stale entry would then bind the dead view and draw as a solid block or garbage, depending on the driver.
        // Dropping every entry the frame does not use means a texture that goes away for even one frame cannot leave
        // its bind group behind; it also keeps the cache from growing with every texture the UI has ever shown.
        for (UiQuadFrameResources::BindingCacheEntry &entry : resources.binding_cache) {
            entry.used = false;
        }
        const auto find_entry = [&](RHI::TextureViewHandle view, u64 revision) {
            return std::ranges::find_if(resources.binding_cache, [&](const auto &entry) {
                return entry.texture_view == view && entry.texture_revision == revision;
            });
        };
        for (UiQuadDrawBatch &batch : out_batches) {
            batch.instance_buffer = resources.instance_buffer;
            auto cached = find_entry(batch.texture_view, batch.texture_revision);
            if (cached == resources.binding_cache.end()) {
                UiQuadFrameResources::BindingCacheEntry entry{.texture_view = batch.texture_view,
                                                              .texture_revision = batch.texture_revision};
                vector<LayoutEntry> entries;
                shared_entries(entries);
                entries.push_back(LayoutEntry{texture_binding_.layout_index,
                                              RHI::BindGroupEntry{.binding = texture_binding_.binding,
                                                                  .texture_view = batch.texture_view}});
                if (Core::RendererResult built = build_groups(entries, 0, "persistent ui quad bind group", entry.bind_groups,
                                                              entry.bind_group_count);
                    !built) {
                    out_batches.clear();
                    return built;
                }
                resources.binding_cache.push_back(std::move(entry));
                cached = std::prev(resources.binding_cache.end());
            }
            cached->used = true;
            batch.bind_groups = cached->bind_groups;
            batch.bind_group_count = cached->bind_group_count;
        }
        for (auto it = resources.binding_cache.begin(); it != resources.binding_cache.end();) {
            if (it->used) {
                ++it;
                continue;
            }
            for (u32 i = 0; i < it->bind_group_count; ++i) {
                if (it->bind_groups[i].handle) {
                    device.destroy_bind_group(it->bind_groups[i].handle);
                }
            }
            it = resources.binding_cache.erase(it);
        }
        resources.stats.textures = static_cast<u32>(resources.binding_cache.size());
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
    Core::RendererResult UiQuadPipeline::draw(RHI::RenderPassEncoder &pass, span<const UiQuadDrawBatch> batches,
                                              glm::vec2 viewport_size, UiQuadDrawState *state) {
        if (batches.empty()) {
            return {};
        }
        if (!pipeline_ || !additive_pipeline_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, "UI quad pipeline was not created.");
        }
        UiQuadDrawState local;
        UiQuadDrawState &bound = state != nullptr ? *state : local;

        for (const UiQuadDrawBatch &batch : batches) {
            if (!batch.instance_buffer) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "UI quad draw batch has no prepared instance buffer.");
            }
            const RHI::RenderPipelineHandle wanted = batch.additive ? additive_pipeline_ : pipeline_;
            if (bound.pipeline != wanted) {
                pass.set_pipeline(wanted);
                bound.pipeline = wanted;
            }
            if (!bound.scissor_set || !same_rect(bound.scissor, batch.scissor)) {
                pass.set_scissor(batch.scissor);
                bound.scissor = batch.scissor;
                bound.scissor_set = true;
            }
            for (u32 i = 0; i < batch.bind_group_count; ++i) {
                const UiQuadDrawBatch::BoundGroup &group = batch.bind_groups[i];
                if (!group.handle) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "UI quad draw batch has an invalid persistent bind group.");
                }
                UiQuadDrawBatch::BoundGroup *slot = nullptr;
                for (u32 k = 0; k < bound.bind_group_count; ++k) {
                    if (bound.bind_groups[k].set == group.set) {
                        slot = &bound.bind_groups[k];
                        break;
                    }
                }
                if (slot != nullptr && slot->handle == group.handle) {
                    continue;
                }
                pass.set_bind_group(group.set, group.handle);
                if (slot != nullptr) {
                    slot->handle = group.handle;
                } else if (bound.bind_group_count < kUiQuadMaxBindGroups) {
                    bound.bind_groups[bound.bind_group_count++] = group;
                }
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
    void UiQuadPipeline::destroy(RHI::RhiDevice &device) noexcept {
        if (pipeline_) {
            device.destroy_render_pipeline(pipeline_);
        }
        if (additive_pipeline_) {
            device.destroy_render_pipeline(additive_pipeline_);
        }
        if (nearest_sampler_) {
            device.destroy_sampler(nearest_sampler_);
            nearest_sampler_ = {};
        }
        if (sampler_) {
            device.destroy_sampler(sampler_);
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
        *this = UiQuadPipeline{};
    }

} // namespace SFT::UI
