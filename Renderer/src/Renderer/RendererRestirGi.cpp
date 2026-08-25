#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <atomic>
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
#include <Renderer/RestirGi.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied ReSTIR-GI failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError restir_gi_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        /// Finds the reflected binding with the supplied name, or reports a ReSTIR-GI error.
        ///
        /// @param resources Reflected resource bindings to search.
        /// @param name Name of the shader resource being resolved.
        /// @param shader_label Label used in the error message when the resource is missing.
        ///
        /// @return Returns the binding index on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<u32> find_restir_binding(
            const vector<ReflectedResource> &resources, const char *name, const char *shader_label) {
            for (const ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    return resource.binding;
                }
            }
            return unexpected(restir_gi_error(
                string(shader_label) + " shader reflection is missing the '" + name + "' resource."));
        }

    } // namespace

    /// Finds or creates the ReSTIR GI resources required by the operation, (re)allocating the persistent
    /// reservoir buffers and history texture whenever the render extent changes.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_restir_gi_resources(glm::uvec2 render_extent) {
        ZoneScopedN("Renderer::ensure_restir_gi_resources");
        render_extent.x = std::max(render_extent.x, 1u);
        render_extent.y = std::max(render_extent.y, 1u);

        auto guard = restir_gi_.lock();
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(restir_gi_error("Cannot build ReSTIR GI resources without an RHI device."));
        }

        if (!guard->ready) {
            const auto shader_target = shader_target_for_device(*device);
            if (!shader_target) return unexpected(shader_target.error());
            const bool disk_cache = recovery_create_info_.enable_shader_disk_cache;

            auto cleanup = [&]() noexcept {
                const auto destroy_variant = [&](RestirGiComputeVariant &variant) noexcept {
                    if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                    if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                    if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                    if (variant.module) device->destroy_shader_module(variant.module);
                    variant = {};
                };
                destroy_variant(guard->initial_sample);
                destroy_variant(guard->temporal_reuse);
                destroy_variant(guard->spatial_reuse);
                destroy_variant(guard->shade_resolve);
                destroy_variant(guard->history_copy);
                if (guard->linear_sampler) device->destroy_sampler(guard->linear_sampler);
                if (guard->atmosphere_sampler) device->destroy_sampler(guard->atmosphere_sampler);
                guard->linear_sampler = {};
                guard->atmosphere_sampler = {};
            };

            struct VariantSpec {
                const char *shader_path;
                const char *module_name;
                const char *entry_point;
                const char *label;
                RestirGiComputeVariant *variant;
            };
            const array<VariantSpec, 5> specs{
                VariantSpec{"Shaders/restir_gi_initial_sample.slang", "restir_gi_initial_sample", "initialSampleMain",
                            "ReSTIR GI initial-sample pipeline", &guard->initial_sample},
                VariantSpec{"Shaders/restir_gi_temporal_reuse.slang", "restir_gi_temporal_reuse", "temporalReuseMain",
                            "ReSTIR GI temporal-reuse pipeline", &guard->temporal_reuse},
                VariantSpec{"Shaders/restir_gi_spatial_reuse.slang", "restir_gi_spatial_reuse", "spatialReuseMain",
                            "ReSTIR GI spatial-reuse pipeline", &guard->spatial_reuse},
                VariantSpec{"Shaders/restir_gi_shade_resolve.slang", "restir_gi_shade_resolve", "shadeResolveMain",
                            "ReSTIR GI shade-resolve pipeline", &guard->shade_resolve},
                VariantSpec{"Shaders/restir_gi_history_copy.slang", "restir_gi_history_copy", "historyCopyMain",
                            "ReSTIR GI history-copy pipeline", &guard->history_copy},
            };

            for (const VariantSpec &spec : specs) {
                RestirGiComputeVariant &variant = *spec.variant;

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
                    return unexpected(restir_gi_error(
                        string("compile ") + spec.label + " failed: " + shader.error().message + "\n" + shader.error().diagnostics));
                }
                variant.shader = *shader;

                auto code = variant.shader.entry_point_code(spec.entry_point, shader_target->slang_target.format);
                if (!code) {
                    cleanup();
                    return unexpected(restir_gi_error(string("generate ") + spec.label + " bytecode failed: " + code.error().message));
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
                    return unexpected(restir_gi_error(string(spec.label) + " shader reflection produced no bind-group layout."));
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

            const RHI::SamplerDesc linear_sampler_desc{
                .min_filter = RHI::Filter::Linear,
                .mag_filter = RHI::Filter::Linear,
                .mipmap_mode = RHI::MipmapMode::Nearest,
                .address_u = RHI::AddressMode::ClampToEdge,
                .address_v = RHI::AddressMode::ClampToEdge,
                .address_w = RHI::AddressMode::ClampToEdge,
                .max_lod = 0.0f,
                .label = "ReSTIR GI linear sampler",
            };
            auto linear_sampler = device->create_sampler(linear_sampler_desc);
            if (!linear_sampler) {
                cleanup();
                return unexpected(graphics_error_from_rhi(linear_sampler.error(), "create ReSTIR GI linear sampler"));
            }
            guard->linear_sampler = *linear_sampler;

            const RHI::SamplerDesc atmosphere_sampler_desc{
                .min_filter = RHI::Filter::Linear,
                .mag_filter = RHI::Filter::Linear,
                .mipmap_mode = RHI::MipmapMode::Nearest,
                .address_u = RHI::AddressMode::ClampToEdge,
                .address_v = RHI::AddressMode::ClampToEdge,
                .address_w = RHI::AddressMode::ClampToEdge,
                .max_lod = 0.0f,
                .label = "ReSTIR GI atmosphere lut sampler",
            };
            auto atmosphere_sampler = device->create_sampler(atmosphere_sampler_desc);
            if (!atmosphere_sampler) {
                cleanup();
                return unexpected(graphics_error_from_rhi(atmosphere_sampler.error(), "create ReSTIR GI atmosphere lut sampler"));
            }
            guard->atmosphere_sampler = *atmosphere_sampler;

            guard->ready = true;
        }

        if (guard->reservoir_extent_x == render_extent.x && guard->reservoir_extent_y == render_extent.y) {
            return {};
        }

        if (guard->reservoir_buffer_a) device->destroy_buffer(guard->reservoir_buffer_a);
        if (guard->reservoir_buffer_b) device->destroy_buffer(guard->reservoir_buffer_b);
        if (guard->reservoir_buffer_spatial) device->destroy_buffer(guard->reservoir_buffer_spatial);
        if (guard->guide_buffer_a) device->destroy_buffer(guard->guide_buffer_a);
        if (guard->guide_buffer_b) device->destroy_buffer(guard->guide_buffer_b);
        if (guard->previous_scene_color_view) device->destroy_texture_view(guard->previous_scene_color_view);
        if (guard->previous_scene_color_texture) device->destroy_texture(guard->previous_scene_color_texture);
        guard->reservoir_buffer_a = {};
        guard->reservoir_buffer_b = {};
        guard->reservoir_buffer_spatial = {};
        guard->guide_buffer_a = {};
        guard->guide_buffer_b = {};
        guard->previous_scene_color_view = {};
        guard->previous_scene_color_texture = {};

        const u64 pixel_count = static_cast<u64>(render_extent.x) * static_cast<u64>(render_extent.y);
        const u64 reservoir_buffer_size = pixel_count * sizeof(ReservoirGpuData);

        const auto create_reservoir_buffer = [&](const char *label) -> Core::RendererExpected<RHI::BufferHandle> {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = reservoir_buffer_size,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::DeviceLocal,
                .label = label,
            });
            if (!buffer) return unexpected(graphics_error_from_rhi(buffer.error(), label));
            return *buffer;
        };

        auto buffer_a = create_reservoir_buffer("ReSTIR GI reservoir buffer A");
        if (!buffer_a) return unexpected(buffer_a.error());
        auto buffer_b = create_reservoir_buffer("ReSTIR GI reservoir buffer B");
        if (!buffer_b) {
            device->destroy_buffer(*buffer_a);
            return unexpected(buffer_b.error());
        }
        auto buffer_spatial = create_reservoir_buffer("ReSTIR GI reservoir buffer spatial");
        if (!buffer_spatial) {
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            return unexpected(buffer_spatial.error());
        }

        const u64 guide_buffer_size = pixel_count * sizeof(GuideGpuData);
        const auto create_guide_buffer = [&](const char *label) -> Core::RendererExpected<RHI::BufferHandle> {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = guide_buffer_size,
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::DeviceLocal,
                .label = label,
            });
            if (!buffer) return unexpected(graphics_error_from_rhi(buffer.error(), label));
            return *buffer;
        };
        auto guide_a = create_guide_buffer("ReSTIR GI guide buffer A");
        if (!guide_a) {
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            return unexpected(guide_a.error());
        }
        auto guide_b = create_guide_buffer("ReSTIR GI guide buffer B");
        if (!guide_b) {
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            return unexpected(guide_b.error());
        }

        auto history_texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::RGBA16Float,
            .extent = RHI::Extent3D{.width = render_extent.x, .height = render_extent.y, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "ReSTIR GI previous scene color history",
        });
        if (!history_texture) {
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(history_texture.error(), "create ReSTIR GI history texture"));
        }
        auto history_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *history_texture,
            .view_type = RHI::TextureViewType::View2D,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .label = "ReSTIR GI history texture view",
        });
        if (!history_view) {
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(history_view.error(), "create ReSTIR GI history texture view"));
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "ReSTIR GI buffer clear"});
        if (!encoder) {
            device->destroy_texture_view(*history_view);
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(encoder.error(), "create ReSTIR GI buffer clear encoder"));
        }
        (*encoder)->fill_buffer(*buffer_a, 0, reservoir_buffer_size, 0u);
        (*encoder)->fill_buffer(*buffer_b, 0, reservoir_buffer_size, 0u);
        (*encoder)->fill_buffer(*buffer_spatial, 0, reservoir_buffer_size, 0u);
        (*encoder)->fill_buffer(*guide_a, 0, guide_buffer_size, 0u);
        (*encoder)->fill_buffer(*guide_b, 0, guide_buffer_size, 0u);
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            device->destroy_texture_view(*history_view);
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish ReSTIR GI buffer clear"));
        }
        auto fence = device->create_fence(RHI::FenceDesc{.label = "ReSTIR GI buffer clear fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            device->destroy_texture_view(*history_view);
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(fence.error(), "create ReSTIR GI buffer clear fence"));
        }
        const array command_buffers{*command_buffer};
        const RHI::SubmitDesc submit{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "ReSTIR GI buffer clear submit",
        };
        if (auto submitted = device->submit(submit); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            device->destroy_texture_view(*history_view);
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit ReSTIR GI buffer clear"));
        }
        auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        if (!waited || !*waited) {
            device->destroy_texture_view(*history_view);
            device->destroy_texture(*history_texture);
            device->destroy_buffer(*buffer_a);
            device->destroy_buffer(*buffer_b);
            device->destroy_buffer(*buffer_spatial);
            device->destroy_buffer(*guide_a);
            device->destroy_buffer(*guide_b);
            return unexpected(restir_gi_error("wait ReSTIR GI buffer clear: fence wait failed or timed out."));
        }

        guard->reservoir_buffer_a = *buffer_a;
        guard->reservoir_buffer_b = *buffer_b;
        guard->reservoir_buffer_spatial = *buffer_spatial;
        guard->guide_buffer_a = *guide_a;
        guard->guide_buffer_b = *guide_b;
        guard->previous_scene_color_texture = *history_texture;
        guard->previous_scene_color_view = *history_view;
        guard->reservoir_extent_x = render_extent.x;
        guard->reservoir_extent_y = render_extent.y;
        guard->previous_is_a = false;
        // History is meaningless immediately after a (re)allocation — every reservoir slot and the
        // history texture were just cleared to zero, so the first frame after this must not read them.
        guard->has_history = false;
        return {};
    }

    /// Destroys the ReSTIR GI resources identified by the supplied parameters.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_restir_gi_resources() noexcept {
        ZoneScopedN("Renderer::destroy_restir_gi_resources");
        RHI::RhiDevice *device = rhi_device();
        auto guard = restir_gi_.lock();
        const auto destroy_variant = [&](RestirGiComputeVariant &variant) noexcept {
            if (device != nullptr) {
                if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                if (variant.module) device->destroy_shader_module(variant.module);
            }
            variant = {};
        };
        destroy_variant(guard->initial_sample);
        destroy_variant(guard->temporal_reuse);
        destroy_variant(guard->spatial_reuse);
        destroy_variant(guard->shade_resolve);
        destroy_variant(guard->history_copy);
        if (device != nullptr) {
            if (guard->reservoir_buffer_a) device->destroy_buffer(guard->reservoir_buffer_a);
            if (guard->reservoir_buffer_b) device->destroy_buffer(guard->reservoir_buffer_b);
            if (guard->reservoir_buffer_spatial) device->destroy_buffer(guard->reservoir_buffer_spatial);
            if (guard->guide_buffer_a) device->destroy_buffer(guard->guide_buffer_a);
            if (guard->guide_buffer_b) device->destroy_buffer(guard->guide_buffer_b);
            if (guard->previous_scene_color_view) device->destroy_texture_view(guard->previous_scene_color_view);
            if (guard->previous_scene_color_texture) device->destroy_texture(guard->previous_scene_color_texture);
            if (guard->linear_sampler) device->destroy_sampler(guard->linear_sampler);
            if (guard->atmosphere_sampler) device->destroy_sampler(guard->atmosphere_sampler);
        }
        *guard = {};
    }

    /// Records the ReSTIR GI initial-candidate-sample pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_restir_gi_initial_sample(
        RHI::ComputePassEncoder &pass,
        FrameInFlight &slot,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::TextureViewHandle gbuffer_albedo_view,
        RHI::TextureViewHandle gbuffer_material_view,
        RHI::TextureViewHandle gbuffer_emissive_view,
        RHI::TextureViewHandle gbuffer_motion_view,
        RHI::TextureViewHandle transmittance_lut_view,
        RHI::TextureViewHandle sky_view_lut_view,
        RHI::BufferHandle atmosphere_constants,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_restir_gi_initial_sample");
        if (Core::RendererResult ready = ensure_restir_gi_resources(render_extent); !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer || !slot.scene_tlas) {
            return unexpected(restir_gi_error("Cannot record ReSTIR GI initial-sample without constants/TLAS/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle current_buffer{};
        RHI::BufferHandle guide_prev_buffer{};
        RHI::BufferHandle guide_current_buffer{};
        RHI::TextureViewHandle history_view{};
        RHI::SamplerHandle linear_sampler{};
        RHI::SamplerHandle atmosphere_sampler{};
        {
            auto guard = restir_gi_.lock();
            layout = guard->initial_sample.bind_group_layout;
            pipeline = guard->initial_sample.pipeline;
            resources = guard->initial_sample.resources;
            current_buffer = guard->previous_is_a ? guard->reservoir_buffer_b : guard->reservoir_buffer_a;
            // Guide buffers ping-pong in lockstep with the reservoir buffers.
            guide_prev_buffer = guard->previous_is_a ? guard->guide_buffer_a : guard->guide_buffer_b;
            guide_current_buffer = guard->previous_is_a ? guard->guide_buffer_b : guard->guide_buffer_a;
            history_view = guard->previous_scene_color_view;
            linear_sampler = guard->linear_sampler;
            atmosphere_sampler = guard->atmosphere_sampler;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI initial-sample");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI initial-sample");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        const auto bind_sampler = [&](const char *name, RHI::SamplerHandle sampler) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI initial-sample");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .sampler = sampler});
            return {};
        };
        const auto bind_acceleration_structure = [&](const char *name, RHI::AccelerationStructureHandle handle) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI initial-sample");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .acceleration_structure = handle});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("atmosphereData", atmosphere_constants); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferAlbedo", gbuffer_albedo_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferMaterial", gbuffer_material_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferEmissive", gbuffer_emissive_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferMotion", gbuffer_motion_view); !r.has_value()) return r;
        if (auto r = bind_texture("previousSceneColor", history_view); !r.has_value()) return r;
        if (auto r = bind_texture("transmittanceLut", transmittance_lut_view); !r.has_value()) return r;
        if (auto r = bind_texture("skyViewLut", sky_view_lut_view); !r.has_value()) return r;
        if (auto r = bind_sampler("linearSampler", linear_sampler); !r.has_value()) return r;
        if (auto r = bind_sampler("atmosphereSampler", atmosphere_sampler); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirCandidate", current_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("guideDirectionPrev", guide_prev_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("guideDirectionCurrent", guide_current_buffer); !r.has_value()) return r;
        if (auto r = bind_acceleration_structure("sceneAccelerationStructure", slot.scene_tlas); !r.has_value()) return r;
        if (auto r = bind_buffer("geometryVertices", vertex_arena_.buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("geometryIndices", index_arena_.buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("sceneInstances", slot.spectral_scene_instances); !r.has_value()) return r;
        if (auto r = bind_buffer("sceneMaterials", slot.spectral_materials); !r.has_value()) return r;

        // Mirrors RendererSpectralPathTracing.cpp's append_material_texture_heap() so material sampling
        // through sturdy_spectral_scene.slang's bindless heap works identically here.
        {
            auto binding = find_restir_binding(resources, "spectralMaterialTextures", "ReSTIR GI initial-sample");
            if (!binding) return unexpected(binding.error());
            for (u32 index = 0; index < slot.spectral_material_textures.size(); ++index) {
                const TextureResource *material_texture = texture(slot.spectral_material_textures[index]);
                if (material_texture == nullptr || !material_texture->view || !material_texture->sampler) {
                    return unexpected(restir_gi_error("ReSTIR GI initial-sample material texture heap entry is invalid."));
                }
                entries.push_back(RHI::BindGroupEntry{
                    .binding = *binding,
                    .array_element = index,
                    .texture_view = material_texture->view,
                    .sampler = material_texture->sampler,
                });
            }
        }

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .variable_descriptor_count = static_cast<u32>(slot.spectral_material_textures.size()),
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "ReSTIR GI initial-sample bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create ReSTIR GI initial-sample bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        (void)settings;
        return {};
    }

    /// Records the ReSTIR GI temporal reuse pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_restir_gi_temporal_reuse(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_motion_view,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_restir_gi_temporal_reuse");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(restir_gi_error("Cannot record ReSTIR GI temporal-reuse without constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle current_buffer{};
        RHI::BufferHandle previous_buffer{};
        {
            auto guard = restir_gi_.lock();
            layout = guard->temporal_reuse.bind_group_layout;
            pipeline = guard->temporal_reuse.pipeline;
            resources = guard->temporal_reuse.resources;
            current_buffer = guard->previous_is_a ? guard->reservoir_buffer_b : guard->reservoir_buffer_a;
            previous_buffer = guard->previous_is_a ? guard->reservoir_buffer_a : guard->reservoir_buffer_b;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI temporal-reuse");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI temporal-reuse");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferMotion", gbuffer_motion_view); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirPrevious", previous_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirCurrent", current_buffer); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "ReSTIR GI temporal-reuse bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create ReSTIR GI temporal-reuse bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        (void)settings;
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        u32 dispatch_x = 0;
        u32 dispatch_y = 0;
        {
            auto guard = restir_gi_.lock();
            dispatch_x = (guard->reservoir_extent_x + 7u) / 8u;
            dispatch_y = (guard->reservoir_extent_y + 7u) / 8u;
        }
        pass.dispatch(dispatch_x, dispatch_y, 1);
        return {};
    }

    /// Records the ReSTIR GI spatial reuse pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_restir_gi_spatial_reuse(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_restir_gi_spatial_reuse");
        (void)settings;
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(restir_gi_error("Cannot record ReSTIR GI spatial-reuse without constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle current_buffer{};
        RHI::BufferHandle spatial_buffer{};
        u32 extent_x = 0;
        u32 extent_y = 0;
        {
            auto guard = restir_gi_.lock();
            layout = guard->spatial_reuse.bind_group_layout;
            pipeline = guard->spatial_reuse.pipeline;
            resources = guard->spatial_reuse.resources;
            current_buffer = guard->previous_is_a ? guard->reservoir_buffer_b : guard->reservoir_buffer_a;
            spatial_buffer = guard->reservoir_buffer_spatial;
            extent_x = guard->reservoir_extent_x;
            extent_y = guard->reservoir_extent_y;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI spatial-reuse");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI spatial-reuse");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirCurrent", current_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirSpatial", spatial_buffer); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "ReSTIR GI spatial-reuse bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create ReSTIR GI spatial-reuse bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((extent_x + 7u) / 8u, (extent_y + 7u) / 8u, 1);
        return {};
    }

    /// Records the ReSTIR GI shade-resolve pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_restir_gi_shade_resolve(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::TextureViewHandle gbuffer_albedo_view,
        RHI::TextureViewHandle gbuffer_material_view,
        RHI::TextureViewHandle output_view,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_restir_gi_shade_resolve");
        (void)settings;
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer || !output_view) {
            return unexpected(restir_gi_error("Cannot record ReSTIR GI shade-resolve without constants/output/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle spatial_buffer{};
        {
            auto guard = restir_gi_.lock();
            layout = guard->shade_resolve.bind_group_layout;
            pipeline = guard->shade_resolve.pipeline;
            resources = guard->shade_resolve.resources;
            spatial_buffer = guard->reservoir_buffer_spatial;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI shade-resolve");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI shade-resolve");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferAlbedo", gbuffer_albedo_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferMaterial", gbuffer_material_view); !r.has_value()) return r;
        if (auto r = bind_buffer("reservoirSpatial", spatial_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("irradianceOut", output_view); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "ReSTIR GI shade-resolve bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create ReSTIR GI shade-resolve bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Dispatches to the ReSTIR GI denoiser backend selected by `RestirGiSettings::denoiser`. This is
    /// the pluggable-denoiser seam: `None` is a zero-cost passthrough, `Svgf` is implemented today, and
    /// `DlssRayReconstruction`/`FsrRedstone` are reserved values that currently fall back to `Svgf` with
    /// a one-time warning — wiring up either vendor SDK later means adding one
    /// `build_<name>_denoiser_module` function with this same signature and one new switch case here,
    /// with no changes required anywhere else in the ReSTIR GI pipeline.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RenderGraphTextureHandle> Renderer::build_restir_gi_denoiser_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &slot,
        const RestirGiDenoiserInputs &inputs) {
        ZoneScopedN("Renderer::build_restir_gi_denoiser_module");
        // Mirrors Renderer::RestirGiSettings::denoiser's documented wire values (Scene.hpp); the
        // Renderer package intentionally uses plain integers rather than Engine::RestirGiDenoiser here
        // so it does not need to depend on the Engine layer's enum type.
        enum : u32 { kDenoiserNone = 0, kDenoiserSvgf = 1, kDenoiserDlssRayReconstruction = 2, kDenoiserFsrRedstone = 3 };
        switch (submission.render_graph.restir_gi.denoiser) {
            case kDenoiserNone:
                return inputs.raw_irradiance;
            case kDenoiserDlssRayReconstruction:
            case kDenoiserFsrRedstone: {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    Foundation::log_warn(
                        "ReSTIR GI denoiser {} is not implemented yet; falling back to SVGF.",
                        submission.render_graph.restir_gi.denoiser == kDenoiserDlssRayReconstruction
                            ? "DLSS Ray Reconstruction" : "FSR Redstone");
                }
                return build_svgf_denoiser_module(context, submission, slot, inputs);
            }
            case kDenoiserSvgf:
            default:
                return build_svgf_denoiser_module(context, submission, slot, inputs);
        }
    }

    /// Records the end-of-frame copy of this frame's final scene color into ReSTIR GI's history texture.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_restir_gi_history_copy(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle scene_color_view,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_restir_gi_history_copy");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !scene_color_view) {
            return unexpected(restir_gi_error("Cannot record ReSTIR GI history-copy without scene color/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::TextureViewHandle history_view{};
        u32 extent_x = 0;
        u32 extent_y = 0;
        {
            auto guard = restir_gi_.lock();
            layout = guard->history_copy.bind_group_layout;
            pipeline = guard->history_copy.pipeline;
            resources = guard->history_copy.resources;
            history_view = guard->previous_scene_color_view;
            extent_x = guard->reservoir_extent_x;
            extent_y = guard->reservoir_extent_y;
            // Every reservoir/history buffer now holds at least one full frame of real data.
            guard->has_history = true;
            guard->previous_is_a = !guard->previous_is_a;
        }
        if (!history_view) {
            return unexpected(restir_gi_error("ReSTIR GI history texture is not allocated."));
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_restir_binding(resources, name, "ReSTIR GI history-copy");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_texture("sceneColorIn", scene_color_view); !r.has_value()) return r;
        if (auto r = bind_texture("historyOut", history_view); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "ReSTIR GI history-copy bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create ReSTIR GI history-copy bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((extent_x + 7u) / 8u, (extent_y + 7u) / 8u, 1);
        return {};
    }

} // namespace SFT::Renderer
