#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/SvgfDenoiser.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied SVGF failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError svgf_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        /// Finds the reflected binding with the supplied name, or reports an SVGF error.
        ///
        /// @param resources Reflected resource bindings to search.
        /// @param name Name of the shader resource being resolved.
        /// @param shader_label Label used in the error message when the resource is missing.
        ///
        /// @return Returns the binding index on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<u32> find_svgf_binding(
            const vector<ReflectedResource> &resources, const char *name, const char *shader_label) {
            for (const ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    return resource.binding;
                }
            }
            return unexpected(svgf_error(
                string(shader_label) + " shader reflection is missing the '" + name + "' resource."));
        }

    } // namespace

    /// Finds or creates the SVGF resources required by the operation, (re)allocating the persistent
    /// history textures whenever the render extent changes.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_svgf_resources(glm::uvec2 render_extent) {
        ZoneScopedN("Renderer::ensure_svgf_resources");
        render_extent.x = std::max(render_extent.x, 1u);
        render_extent.y = std::max(render_extent.y, 1u);

        auto guard = svgf_denoiser_.lock();
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(svgf_error("Cannot build SVGF resources without an RHI device."));
        }

        if (!guard->ready) {
            const auto shader_target = shader_target_for_device(*device);
            if (!shader_target) return unexpected(shader_target.error());
            const bool disk_cache = recovery_create_info_.enable_shader_disk_cache;

            auto cleanup = [&]() noexcept {
                const auto destroy_variant = [&](SvgfComputeVariant &variant) noexcept {
                    if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                    if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                    if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                    if (variant.module) device->destroy_shader_module(variant.module);
                    variant = {};
                };
                destroy_variant(guard->temporal_accumulate);
                destroy_variant(guard->atrous);
            };

            struct VariantSpec {
                const char *shader_path;
                const char *module_name;
                const char *entry_point;
                const char *label;
                SvgfComputeVariant *variant;
            };
            const array<VariantSpec, 2> specs{
                VariantSpec{"Shaders/svgf_temporal_accumulate.slang", "svgf_temporal_accumulate", "temporalAccumulateMain",
                            "SVGF temporal-accumulate pipeline", &guard->temporal_accumulate},
                VariantSpec{"Shaders/svgf_atrous.slang", "svgf_atrous", "atrousMain",
                            "SVGF a-trous pipeline", &guard->atrous},
            };

            for (const VariantSpec &spec : specs) {
                SvgfComputeVariant &variant = *spec.variant;

                const slang::ShaderCompileOptions options{
                    .targets = shader_compile_targets_for_device(device),
                    .entry_points = {slang::ShaderEntryPointRequest{.name = spec.entry_point, .stage = slang::ShaderStage::Compute}},
                };
                slang::ShaderVariantCache shader_cache{
                    slang::ShaderSource::from_file(spec.shader_path, spec.module_name),
                    options,
                    slang::ShaderCompiler{},
                    disk_cache};
                auto shader = shader_cache.get_or_compile_base();
                if (!shader) {
                    cleanup();
                    return unexpected(svgf_error(
                        string("compile ") + spec.label + " failed: " + shader.error().message + "\n" + shader.error().diagnostics));
                }
                variant.shader = *shader;

                auto code = variant.shader.entry_point_code(spec.entry_point, shader_target->slang_target.format);
                if (!code) {
                    cleanup();
                    return unexpected(svgf_error(string("generate ") + spec.label + " bytecode failed: " + code.error().message));
                }
                auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                    .language = shader_target->module_language,
                    .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                    .label = spec.label,
                });
                if (!module) {
                    cleanup();
                    return unexpected(graphics_error_from_rhi(module.error(), (string("create ") + spec.label + " module").c_str()));
                }
                variant.module = *module;

                const slang::ShaderReflection &reflection = variant.shader.reflection();
                const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
                if (generated.empty()) {
                    cleanup();
                    return unexpected(svgf_error(string(spec.label) + " shader reflection produced no bind-group layout."));
                }
                auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                    .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                    .label = spec.label,
                });
                if (!layout) {
                    cleanup();
                    return unexpected(graphics_error_from_rhi(layout.error(), (string("create ") + spec.label + " bind group layout").c_str()));
                }
                variant.bind_group_layout = *layout;
                variant.resources = collect_resource_bindings(reflection);

                const vector<RHI::PushConstantRange> push_constant_ranges =
                    generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
                const array<RHI::BindGroupLayoutHandle, 1> layouts{variant.bind_group_layout};
                auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                    .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                    .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                    .label = spec.label,
                });
                if (!pipeline_layout) {
                    cleanup();
                    return unexpected(graphics_error_from_rhi(pipeline_layout.error(), (string("create ") + spec.label + " pipeline layout").c_str()));
                }
                variant.pipeline_layout = *pipeline_layout;

                auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                    .layout = variant.pipeline_layout,
                    .compute = RHI::ShaderEntry{.module = variant.module, .entry_point = spec.entry_point, .stage = RHI::ShaderStage::Compute},
                    .label = spec.label,
                });
                if (!pipeline) {
                    cleanup();
                    return unexpected(graphics_error_from_rhi(pipeline.error(), (string("create ") + spec.label + " pipeline").c_str()));
                }
                variant.pipeline = *pipeline;
                variant.shader.release_compiler_state();
            }

            guard->ready = true;
        }

        if (guard->extent_x == render_extent.x && guard->extent_y == render_extent.y) {
            return {};
        }

        const auto destroy_texture = [&](RHI::TextureHandle &texture, RHI::TextureViewHandle &view) noexcept {
            if (view) device->destroy_texture_view(view);
            if (texture) device->destroy_texture(texture);
            texture = {};
            view = {};
        };
        destroy_texture(guard->color_history_a, guard->color_history_a_view);
        destroy_texture(guard->color_history_b, guard->color_history_b_view);
        destroy_texture(guard->moments_history_a, guard->moments_history_a_view);
        destroy_texture(guard->moments_history_b, guard->moments_history_b_view);
        destroy_texture(guard->history_length_a, guard->history_length_a_view);
        destroy_texture(guard->history_length_b, guard->history_length_b_view);

        const auto create_texture = [&](RHI::Format format, const char *label) -> Core::RendererExpected<std::pair<RHI::TextureHandle, RHI::TextureViewHandle>> {
            auto tex = device->create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = format,
                .extent = RHI::Extent3D{.width = render_extent.x, .height = render_extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
                .label = label,
            });
            if (!tex) return unexpected(graphics_error_from_rhi(tex.error(), label));
            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *tex,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = 0,
                .mip_level_count = 1,
                .label = label,
            });
            if (!view) {
                device->destroy_texture(*tex);
                return unexpected(graphics_error_from_rhi(view.error(), label));
            }
            return std::pair{*tex, *view};
        };

        auto color_a = create_texture(RHI::Format::RGBA16Float, "SVGF color history A");
        if (!color_a) return unexpected(color_a.error());
        auto color_b = create_texture(RHI::Format::RGBA16Float, "SVGF color history B");
        if (!color_b) {
            device->destroy_texture_view(color_a->second);
            device->destroy_texture(color_a->first);
            return unexpected(color_b.error());
        }
        auto moments_a = create_texture(RHI::Format::RG16Float, "SVGF moments history A");
        auto moments_b = moments_a ? create_texture(RHI::Format::RG16Float, "SVGF moments history B")
                                    : decltype(create_texture(RHI::Format::RG16Float, "")){};
        auto length_a = (moments_a && moments_b) ? create_texture(RHI::Format::R16Float, "SVGF history length A")
                                                  : decltype(create_texture(RHI::Format::R16Float, "")){};
        auto length_b = (moments_a && moments_b && length_a) ? create_texture(RHI::Format::R16Float, "SVGF history length B")
                                                               : decltype(create_texture(RHI::Format::R16Float, "")){};

        if (!moments_a || !moments_b || !length_a || !length_b) {
            device->destroy_texture_view(color_a->second);
            device->destroy_texture(color_a->first);
            device->destroy_texture_view(color_b->second);
            device->destroy_texture(color_b->first);
            if (moments_a) { device->destroy_texture_view(moments_a->second); device->destroy_texture(moments_a->first); }
            if (moments_b) { device->destroy_texture_view(moments_b->second); device->destroy_texture(moments_b->first); }
            if (length_a) { device->destroy_texture_view(length_a->second); device->destroy_texture(length_a->first); }
            if (!moments_a) return unexpected(moments_a.error());
            if (!moments_b) return unexpected(moments_b.error());
            if (!length_a) return unexpected(length_a.error());
            return unexpected(length_b.error());
        }

        guard->color_history_a = color_a->first;
        guard->color_history_a_view = color_a->second;
        guard->color_history_b = color_b->first;
        guard->color_history_b_view = color_b->second;
        guard->moments_history_a = moments_a->first;
        guard->moments_history_a_view = moments_a->second;
        guard->moments_history_b = moments_b->first;
        guard->moments_history_b_view = moments_b->second;
        guard->history_length_a = length_a->first;
        guard->history_length_a_view = length_a->second;
        guard->history_length_b = length_b->first;
        guard->history_length_b_view = length_b->second;
        guard->extent_x = render_extent.x;
        guard->extent_y = render_extent.y;
        guard->previous_is_a = false;
        // Every history texture was just cleared/reallocated, so next frame must not reproject from it.
        guard->has_history = false;
        return {};
    }

    /// Destroys the SVGF resources identified by the supplied parameters.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_svgf_resources() noexcept {
        ZoneScopedN("Renderer::destroy_svgf_resources");
        RHI::RhiDevice *device = rhi_device();
        auto guard = svgf_denoiser_.lock();
        const auto destroy_variant = [&](SvgfComputeVariant &variant) noexcept {
            if (device != nullptr) {
                if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                if (variant.module) device->destroy_shader_module(variant.module);
            }
            variant = {};
        };
        destroy_variant(guard->temporal_accumulate);
        destroy_variant(guard->atrous);
        if (device != nullptr) {
            const auto destroy_texture = [&](RHI::TextureHandle texture, RHI::TextureViewHandle view) {
                if (view) device->destroy_texture_view(view);
                if (texture) device->destroy_texture(texture);
            };
            destroy_texture(guard->color_history_a, guard->color_history_a_view);
            destroy_texture(guard->color_history_b, guard->color_history_b_view);
            destroy_texture(guard->moments_history_a, guard->moments_history_a_view);
            destroy_texture(guard->moments_history_b, guard->moments_history_b_view);
            destroy_texture(guard->history_length_a, guard->history_length_a_view);
            destroy_texture(guard->history_length_b, guard->history_length_b_view);
        }
        *guard = {};
    }

    /// Records the SVGF temporal-accumulate pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_svgf_temporal_accumulate(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle raw_irradiance_view,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::TextureViewHandle gbuffer_motion_view,
        RHI::TextureViewHandle accumulated_out_view,
        RHI::BufferHandle constants_buffer,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_svgf_temporal_accumulate");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(svgf_error("Cannot record SVGF temporal-accumulate without constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::TextureViewHandle color_prev{};
        RHI::TextureViewHandle moments_prev{};
        RHI::TextureViewHandle length_prev{};
        RHI::TextureViewHandle moments_out{};
        RHI::TextureViewHandle length_out{};
        {
            auto guard = svgf_denoiser_.lock();
            layout = guard->temporal_accumulate.bind_group_layout;
            pipeline = guard->temporal_accumulate.pipeline;
            resources = guard->temporal_accumulate.resources;
            color_prev = guard->previous_is_a ? guard->color_history_a_view : guard->color_history_b_view;
            moments_prev = guard->previous_is_a ? guard->moments_history_a_view : guard->moments_history_b_view;
            length_prev = guard->previous_is_a ? guard->history_length_a_view : guard->history_length_b_view;
            moments_out = guard->previous_is_a ? guard->moments_history_b_view : guard->moments_history_a_view;
            length_out = guard->previous_is_a ? guard->history_length_b_view : guard->history_length_a_view;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_svgf_binding(resources, name, "SVGF temporal-accumulate");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_svgf_binding(resources, name, "SVGF temporal-accumulate");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("rawIrradiance", raw_irradiance_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferMotion", gbuffer_motion_view); !r.has_value()) return r;
        if (auto r = bind_texture("colorHistoryPrev", color_prev); !r.has_value()) return r;
        if (auto r = bind_texture("momentsHistoryPrev", moments_prev); !r.has_value()) return r;
        if (auto r = bind_texture("historyLengthPrev", length_prev); !r.has_value()) return r;
        if (auto r = bind_texture("accumulatedOut", accumulated_out_view); !r.has_value()) return r;
        if (auto r = bind_texture("momentsHistoryOut", moments_out); !r.has_value()) return r;
        if (auto r = bind_texture("historyLengthOut", length_out); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "SVGF temporal-accumulate bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create SVGF temporal-accumulate bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Records one SVGF a-trous wavelet filter iteration.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_svgf_atrous(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::TextureViewHandle color_variance_in_view,
        RHI::TextureViewHandle color_variance_out_view,
        RHI::BufferHandle constants_buffer,
        u32 step_size,
        bool write_history,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_svgf_atrous");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(svgf_error("Cannot record SVGF a-trous without constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::TextureViewHandle history_color_out{};
        {
            auto guard = svgf_denoiser_.lock();
            layout = guard->atrous.bind_group_layout;
            pipeline = guard->atrous.pipeline;
            resources = guard->atrous.resources;
            history_color_out = guard->previous_is_a ? guard->color_history_b_view : guard->color_history_a_view;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_svgf_binding(resources, name, "SVGF a-trous");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_svgf_binding(resources, name, "SVGF a-trous");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_texture("colorVarianceIn", color_variance_in_view); !r.has_value()) return r;
        if (auto r = bind_texture("colorVarianceOut", color_variance_out_view); !r.has_value()) return r;
        if (auto r = bind_texture("historyColorOut", history_color_out); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "SVGF a-trous bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create SVGF a-trous bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const SvgfAtrousConstants push_constants{.step_size = step_size, .write_history = write_history ? 1u : 0u};
        pass.set_push_constants(RHI::ShaderStage::Compute, 0, std::as_bytes(span<const SvgfAtrousConstants>{&push_constants, 1}));
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Builds the SVGF denoiser render-graph module.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RenderGraphTextureHandle> Renderer::build_svgf_denoiser_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &slot,
        const RestirGiDenoiserInputs &inputs) {
        ZoneScopedN("Renderer::build_svgf_denoiser_module");
        const RestirGiSettings &settings = submission.render_graph.restir_gi;
        const glm::uvec2 render_extent{context.render_extent.x, context.render_extent.y};

        if (Core::RendererResult ready = ensure_svgf_resources(render_extent); !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed, "Cannot build SVGF without an RHI device."));
        }

        bool has_history = false;
        {
            auto guard = svgf_denoiser_.lock();
            has_history = guard->has_history;
        }

        const glm::mat4 view_projection = submission.camera.projection * submission.camera.view;
        SvgfFrameConstants constants{
            .inverse_view_projection = glm::inverse(view_projection),
            .previous_view_projection = view_projection, // unused by SVGF today; motion vectors drive reprojection
            .extent_history_valid_frame_index = glm::vec4{
                static_cast<f32>(render_extent.x), static_cast<f32>(render_extent.y),
                has_history ? 1.0f : 0.0f, static_cast<f32>(submission.frame_index),
            },
            .temporal_phi_params = glm::vec4{
                settings.svgf_temporal_alpha, settings.svgf_phi_normal, settings.svgf_phi_depth, settings.svgf_phi_luminance,
            },
        };

        auto constant_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(constants),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "SVGF frame constants",
        });
        if (!constant_buffer) {
            return unexpected(graphics_error_from_rhi(constant_buffer.error(), "create SVGF frame constants"));
        }
        if (auto written = device->write_buffer(*constant_buffer, 0, std::as_bytes(span{&constants, 1})); !written) {
            device->destroy_buffer(*constant_buffer);
            return unexpected(graphics_error_from_rhi(written.error(), "write SVGF frame constants"));
        }
        slot.transient_buffers.push_back(*constant_buffer);
        const RHI::BufferHandle constants_buffer = *constant_buffer;

        const RenderGraphTextureHandle accumulated = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "SVGF accumulated color+variance",
        });

        context.graph.add_compute_pass("svgf temporal accumulate"_ustr)
            .add_sampled_texture(inputs.raw_irradiance)
            .add_sampled_texture(inputs.gbuffer_normal)
            .add_sampled_texture(inputs.gbuffer_depth)
            .add_sampled_texture(inputs.gbuffer_motion)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = accumulated, .read = false, .write = true})
            .set_side_effect(true)
            .set_execute([this, &submission, &inputs, accumulated, constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_svgf_temporal_accumulate(
                    graph_context.compute_pass(),
                    graph_context.texture(inputs.raw_irradiance).default_view,
                    graph_context.texture(inputs.gbuffer_normal).default_view,
                    graph_context.texture(inputs.gbuffer_depth).default_view,
                    graph_context.texture(inputs.gbuffer_motion).default_view,
                    graph_context.texture(accumulated).default_view,
                    constants_buffer,
                    render_extent,
                    submission.transient_bind_groups);
            });

        const u32 iterations = std::max(settings.svgf_atrous_iterations, 1u);
        RenderGraphTextureHandle ping = accumulated;
        RenderGraphTextureHandle final_output = accumulated;
        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = RHI::Format::RGBA16Float,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
                .label = "SVGF a-trous output",
            });
            const u32 step_size = 1u << iteration;
            const bool write_history = iteration == 0;
            const RenderGraphTextureHandle iteration_input = ping;

            context.graph.add_compute_pass("svgf atrous"_ustr)
                .add_sampled_texture(inputs.gbuffer_normal)
                .add_sampled_texture(inputs.gbuffer_depth)
                .add_sampled_texture(iteration_input)
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = output, .read = false, .write = true})
                .set_side_effect(true)
                .set_execute([this, &submission, &inputs, iteration_input, output, constants_buffer, step_size,
                              write_history, render_extent](RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                    return record_svgf_atrous(
                        graph_context.compute_pass(),
                        graph_context.texture(inputs.gbuffer_normal).default_view,
                        graph_context.texture(inputs.gbuffer_depth).default_view,
                        graph_context.texture(iteration_input).default_view,
                        graph_context.texture(output).default_view,
                        constants_buffer,
                        step_size,
                        write_history,
                        render_extent,
                        submission.transient_bind_groups);
                });

            ping = output;
            final_output = output;
        }

        {
            auto guard = svgf_denoiser_.lock();
            guard->has_history = true;
            guard->previous_is_a = !guard->previous_is_a;
        }

        return final_output;
    }

} // namespace SFT::Renderer
