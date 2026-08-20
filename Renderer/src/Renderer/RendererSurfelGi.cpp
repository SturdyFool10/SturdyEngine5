#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/SurfelGi.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied surfel-GI failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError surfel_gi_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        struct GridClearConstants {
            u32 bucket_count;
        };
        static_assert(sizeof(GridClearConstants) == 4);

        /// Finds the reflected binding with the supplied name, or reports a surfel-GI error.
        ///
        /// @param resources Reflected resource bindings to search.
        /// @param name Name of the shader resource being resolved.
        /// @param shader_label Label used in the error message when the resource is missing.
        ///
        /// @return Returns the binding index on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<u32> find_surfel_binding(
            const vector<ReflectedResource> &resources, const char *name, const char *shader_label) {
            for (const ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    return resource.binding;
                }
            }
            return unexpected(surfel_gi_error(
                string(shader_label) + " shader reflection is missing the '" + name + "' resource."));
        }

    } // namespace

    /// Finds or creates the surfel-GI resources required by the operation, (re)allocating the persistent
    /// surfel/grid buffers if the requested capacity has grown.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_surfel_gi_resources(u32 surfel_capacity, u32 grid_bucket_capacity) {
        ZoneScopedN("Renderer::ensure_surfel_gi_resources");
        surfel_capacity = std::max(surfel_capacity, 1u);
        grid_bucket_capacity = std::max(grid_bucket_capacity, 1u);

        auto guard = surfel_gi_.lock();
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(surfel_gi_error("Cannot build surfel-GI resources without an RHI device."));
        }

        if (!guard->ready) {
            const auto shader_target = shader_target_for_device(*device);
            if (!shader_target) return unexpected(shader_target.error());
            const bool disk_cache = recovery_create_info_.enable_shader_disk_cache;

            auto cleanup = [&]() noexcept {
                const auto destroy_variant = [&](SurfelGiComputeVariant &variant) noexcept {
                    if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                    if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                    if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                    if (variant.module) device->destroy_shader_module(variant.module);
                    variant = {};
                };
                destroy_variant(guard->screen_seed);
                destroy_variant(guard->grid_clear);
                destroy_variant(guard->hash_grid_build);
                destroy_variant(guard->ray_trace);
                destroy_variant(guard->irradiance_resolve);
            };

            struct VariantSpec {
                const char *shader_path;
                const char *module_name;
                const char *entry_point;
                const char *label;
                SurfelGiComputeVariant *variant;
            };
            const array<VariantSpec, 5> specs{
                VariantSpec{"Shaders/surfel_screen_seed.slang", "surfel_screen_seed", "screenSeedMain",
                            "surfel screen-seed pipeline", &guard->screen_seed},
                VariantSpec{"Shaders/surfel_grid_clear.slang", "surfel_grid_clear", "clearMain",
                            "surfel grid-clear pipeline", &guard->grid_clear},
                VariantSpec{"Shaders/surfel_hash_grid_build.slang", "surfel_hash_grid_build", "hashGridBuildMain",
                            "surfel hash-grid-build pipeline", &guard->hash_grid_build},
                VariantSpec{"Shaders/surfel_ray_trace.slang", "surfel_ray_trace", "rayTraceMain",
                            "surfel ray-trace pipeline", &guard->ray_trace},
                VariantSpec{"Shaders/surfel_irradiance_resolve.slang", "surfel_irradiance_resolve", "irradianceResolveMain",
                            "surfel irradiance-resolve pipeline", &guard->irradiance_resolve},
            };

            for (const VariantSpec &spec : specs) {
                SurfelGiComputeVariant &variant = *spec.variant;

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
                    return unexpected(surfel_gi_error(
                        string("compile ") + spec.label + " failed: " + shader.error().message + "\n" + shader.error().diagnostics));
                }
                variant.shader = *shader;

                auto code = variant.shader.entry_point_code(spec.entry_point, shader_target->slang_target.format);
                if (!code) {
                    cleanup();
                    return unexpected(surfel_gi_error(string("generate ") + spec.label + " bytecode failed: " + code.error().message));
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
                    return unexpected(surfel_gi_error(string(spec.label) + " shader reflection produced no bind-group layout."));
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

        if (guard->surfel_capacity >= surfel_capacity && guard->grid_bucket_capacity >= grid_bucket_capacity) {
            return {};
        }

        if (guard->surfel_buffer) device->destroy_buffer(guard->surfel_buffer);
        if (guard->grid_buffer) device->destroy_buffer(guard->grid_buffer);
        if (guard->alloc_cursor_buffer) device->destroy_buffer(guard->alloc_cursor_buffer);
        guard->surfel_buffer = {};
        guard->grid_buffer = {};
        guard->alloc_cursor_buffer = {};

        auto surfel_buffer = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(surfel_capacity) * sizeof(SurfelGpuData),
            .usage = RHI::BufferUsage::Storage,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "surfel GI surfel buffer",
        });
        if (!surfel_buffer) {
            return unexpected(graphics_error_from_rhi(surfel_buffer.error(), "create surfel GI surfel buffer"));
        }
        auto grid_buffer = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(grid_bucket_capacity) * sizeof(SurfelGridCellGpuData),
            .usage = RHI::BufferUsage::Storage,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "surfel GI hash grid buffer",
        });
        if (!grid_buffer) {
            device->destroy_buffer(*surfel_buffer);
            return unexpected(graphics_error_from_rhi(grid_buffer.error(), "create surfel GI hash grid buffer"));
        }
        auto alloc_cursor_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(u32),
            .usage = RHI::BufferUsage::Storage,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = "surfel GI allocation cursor buffer",
        });
        if (!alloc_cursor_buffer) {
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            return unexpected(graphics_error_from_rhi(alloc_cursor_buffer.error(), "create surfel GI allocation cursor buffer"));
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "surfel GI buffer clear"});
        if (!encoder) {
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            device->destroy_buffer(*alloc_cursor_buffer);
            return unexpected(graphics_error_from_rhi(encoder.error(), "create surfel GI buffer clear encoder"));
        }
        (*encoder)->fill_buffer(*surfel_buffer, 0, static_cast<u64>(surfel_capacity) * sizeof(SurfelGpuData), 0u);
        (*encoder)->fill_buffer(*grid_buffer, 0, static_cast<u64>(grid_bucket_capacity) * sizeof(SurfelGridCellGpuData), 0u);
        (*encoder)->fill_buffer(*alloc_cursor_buffer, 0, sizeof(u32), 0u);
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            device->destroy_buffer(*alloc_cursor_buffer);
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish surfel GI buffer clear"));
        }
        auto fence = device->create_fence(RHI::FenceDesc{.label = "surfel GI buffer clear fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            device->destroy_buffer(*alloc_cursor_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create surfel GI buffer clear fence"));
        }
        const array command_buffers{*command_buffer};
        const RHI::SubmitDesc submit{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "surfel GI buffer clear submit",
        };
        if (auto submitted = device->submit(submit); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            device->destroy_buffer(*alloc_cursor_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit surfel GI buffer clear"));
        }
        auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        if (!waited || !*waited) {
            device->destroy_buffer(*surfel_buffer);
            device->destroy_buffer(*grid_buffer);
            device->destroy_buffer(*alloc_cursor_buffer);
            return unexpected(surfel_gi_error("wait surfel GI buffer clear: fence wait failed or timed out."));
        }

        guard->surfel_buffer = *surfel_buffer;
        guard->grid_buffer = *grid_buffer;
        guard->alloc_cursor_buffer = *alloc_cursor_buffer;
        guard->surfel_capacity = surfel_capacity;
        guard->grid_bucket_capacity = grid_bucket_capacity;
        return {};
    }

    /// Destroys the surfel-GI resources identified by the supplied parameters.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_surfel_gi_resources() noexcept {
        ZoneScopedN("Renderer::destroy_surfel_gi_resources");
        RHI::RhiDevice *device = rhi_device();
        auto guard = surfel_gi_.lock();
        const auto destroy_variant = [&](SurfelGiComputeVariant &variant) noexcept {
            if (device != nullptr) {
                if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                if (variant.module) device->destroy_shader_module(variant.module);
            }
            variant = {};
        };
        destroy_variant(guard->screen_seed);
        destroy_variant(guard->grid_clear);
        destroy_variant(guard->hash_grid_build);
        destroy_variant(guard->ray_trace);
        destroy_variant(guard->irradiance_resolve);
        if (device != nullptr) {
            if (guard->surfel_buffer) device->destroy_buffer(guard->surfel_buffer);
            if (guard->grid_buffer) device->destroy_buffer(guard->grid_buffer);
            if (guard->alloc_cursor_buffer) device->destroy_buffer(guard->alloc_cursor_buffer);
        }
        *guard = {};
    }

    /// Records the surfel screen-space seeding pass (spawns new surfels into coverage gaps).
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_surfel_screen_seed(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_surfel_screen_seed");
        if (Core::RendererResult ready =
                ensure_surfel_gi_resources(settings.surfel_gi.max_surfels, settings.surfel_gi.hash_grid_bucket_count);
            !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !gbuffer_normal_view || !gbuffer_depth_view || !constants_buffer) {
            return unexpected(surfel_gi_error("Cannot record surfel screen-seed without valid views/constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle surfel_buffer{};
        RHI::BufferHandle grid_buffer{};
        RHI::BufferHandle alloc_cursor_buffer{};
        {
            auto guard = surfel_gi_.lock();
            layout = guard->screen_seed.bind_group_layout;
            pipeline = guard->screen_seed.pipeline;
            resources = guard->screen_seed.resources;
            surfel_buffer = guard->surfel_buffer;
            grid_buffer = guard->grid_buffer;
            alloc_cursor_buffer = guard->alloc_cursor_buffer;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel screen-seed");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel screen-seed");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_buffer("surfelBuffer", surfel_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("gridBuffer", grid_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("allocCursor", alloc_cursor_buffer); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "surfel screen-seed bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create surfel screen-seed bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const u32 tiles_x = (render_extent.x + 15u) / 16u;
        const u32 tiles_y = (render_extent.y + 15u) / 16u;
        pass.dispatch(tiles_x, tiles_y, 1);
        return {};
    }

    /// Records the surfel spatial hash-grid clear pass (must run before `record_surfel_hash_grid_build`).
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_surfel_grid_clear(
        RHI::ComputePassEncoder &pass,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_surfel_grid_clear");
        if (Core::RendererResult ready =
                ensure_surfel_gi_resources(settings.surfel_gi.max_surfels, settings.surfel_gi.hash_grid_bucket_count);
            !ready.has_value()) {
            return ready;
        }
        (void)constants_buffer;
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(surfel_gi_error("Cannot record surfel grid-clear without an RHI device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle grid_buffer{};
        u32 grid_bucket_capacity = 0;
        {
            auto guard = surfel_gi_.lock();
            layout = guard->grid_clear.bind_group_layout;
            pipeline = guard->grid_clear.pipeline;
            resources = guard->grid_clear.resources;
            grid_buffer = guard->grid_buffer;
            grid_bucket_capacity = guard->grid_bucket_capacity;
        }

        auto binding = find_surfel_binding(resources, "gridBuffer", "surfel grid-clear");
        if (!binding) return unexpected(binding.error());
        const array<RHI::BindGroupEntry, 1> entries{RHI::BindGroupEntry{.binding = *binding, .buffer = grid_buffer}};
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "surfel grid-clear bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create surfel grid-clear bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const GridClearConstants constants{.bucket_count = grid_bucket_capacity};
        pass.set_push_constants(RHI::ShaderStage::Compute, 0, std::as_bytes(span<const GridClearConstants>{&constants, 1}));
        pass.dispatch((grid_bucket_capacity + 63u) / 64u, 1, 1);
        return {};
    }

    /// Records the surfel spatial hash-grid rebuild pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_surfel_hash_grid_build(
        RHI::ComputePassEncoder &pass,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_surfel_hash_grid_build");
        if (Core::RendererResult ready =
                ensure_surfel_gi_resources(settings.surfel_gi.max_surfels, settings.surfel_gi.hash_grid_bucket_count);
            !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer) {
            return unexpected(surfel_gi_error("Cannot record surfel hash-grid-build without constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle surfel_buffer{};
        RHI::BufferHandle grid_buffer{};
        u32 surfel_capacity = 0;
        {
            auto guard = surfel_gi_.lock();
            layout = guard->hash_grid_build.bind_group_layout;
            pipeline = guard->hash_grid_build.pipeline;
            resources = guard->hash_grid_build.resources;
            surfel_buffer = guard->surfel_buffer;
            grid_buffer = guard->grid_buffer;
            surfel_capacity = guard->surfel_capacity;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel hash-grid-build");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("surfelBuffer", surfel_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("gridBuffer", grid_buffer); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "surfel hash-grid-build bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create surfel hash-grid-build bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((surfel_capacity + 63u) / 64u, 1, 1);
        return {};
    }

    /// Records the surfel ray-trace/temporal-accumulation pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_surfel_ray_trace(
        RHI::ComputePassEncoder &pass,
        FrameInFlight &slot,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_surfel_ray_trace");
        if (Core::RendererResult ready =
                ensure_surfel_gi_resources(settings.surfel_gi.max_surfels, settings.surfel_gi.hash_grid_bucket_count);
            !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !constants_buffer || !slot.scene_tlas) {
            return unexpected(surfel_gi_error("Cannot record surfel ray-trace without constants/TLAS/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle surfel_buffer{};
        u32 surfel_capacity = 0;
        {
            auto guard = surfel_gi_.lock();
            layout = guard->ray_trace.bind_group_layout;
            pipeline = guard->ray_trace.pipeline;
            resources = guard->ray_trace.resources;
            surfel_buffer = guard->surfel_buffer;
            surfel_capacity = guard->surfel_capacity;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel ray-trace");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_acceleration_structure = [&](const char *name, RHI::AccelerationStructureHandle handle) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel ray-trace");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .acceleration_structure = handle});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("surfelBuffer", surfel_buffer); !r.has_value()) return r;
        if (auto r = bind_acceleration_structure("sceneAccelerationStructure", slot.scene_tlas); !r.has_value()) return r;
        if (auto r = bind_buffer("geometryVertices", vertex_arena_.buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("geometryIndices", index_arena_.buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("sceneInstances", slot.spectral_scene_instances); !r.has_value()) return r;
        if (auto r = bind_buffer("sceneMaterials", slot.spectral_materials); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "surfel ray-trace bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create surfel ray-trace bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((surfel_capacity + 63u) / 64u, 1, 1);
        return {};
    }

    /// Records the screen-space surfel irradiance resolve pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_surfel_irradiance_resolve(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle gbuffer_depth_view,
        RHI::TextureViewHandle output_view,
        RHI::BufferHandle constants_buffer,
        const RenderGraphSettings &settings,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_surfel_irradiance_resolve");
        if (Core::RendererResult ready =
                ensure_surfel_gi_resources(settings.surfel_gi.max_surfels, settings.surfel_gi.hash_grid_bucket_count);
            !ready.has_value()) {
            return ready;
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !gbuffer_normal_view || !gbuffer_depth_view || !output_view || !constants_buffer) {
            return unexpected(surfel_gi_error("Cannot record surfel irradiance-resolve without valid views/constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        RHI::BufferHandle surfel_buffer{};
        RHI::BufferHandle grid_buffer{};
        {
            auto guard = surfel_gi_.lock();
            layout = guard->irradiance_resolve.bind_group_layout;
            pipeline = guard->irradiance_resolve.pipeline;
            resources = guard->irradiance_resolve.resources;
            surfel_buffer = guard->surfel_buffer;
            grid_buffer = guard->grid_buffer;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind_buffer = [&](const char *name, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel irradiance-resolve");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer});
            return {};
        };
        const auto bind_texture = [&](const char *name, RHI::TextureViewHandle view) -> Core::RendererResult {
            auto binding = find_surfel_binding(resources, name, "surfel irradiance-resolve");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .texture_view = view});
            return {};
        };
        if (auto r = bind_buffer("frame", constants_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferNormal", gbuffer_normal_view); !r.has_value()) return r;
        if (auto r = bind_texture("gbufferDepth", gbuffer_depth_view); !r.has_value()) return r;
        if (auto r = bind_buffer("surfelBuffer", surfel_buffer); !r.has_value()) return r;
        if (auto r = bind_buffer("gridBuffer", grid_buffer); !r.has_value()) return r;
        if (auto r = bind_texture("irradianceOut", output_view); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "surfel irradiance-resolve bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create surfel irradiance-resolve bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

} // namespace SFT::Renderer
