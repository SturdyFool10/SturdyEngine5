#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/SpectralPathTracing.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

/// Spatial-only XeGTAO-style ground-truth ambient occlusion.
///
/// Three compute passes, scheduled between G-buffer generation and deferred lighting:
///
///   scene depth -> [prefilter] linear view depth + 5-level depth mip chain
///               -> [main]      horizon search -> raw AO + depth edges
///               -> [denoise]   5x5 edge-aware filter -> final AO
///
/// The algorithm lives in Shaders/gtao_{common,prefilter_depth,main,denoise}.slang; this file is
/// resource management and scheduling only, and holds no backend-specific assumptions beyond the
/// engine's RHI vocabulary.
///
/// Everything temporal in XeGTAO is intentionally absent: no accumulation, no reprojection, no
/// frame-index-dependent sampling. This renderer has no TAA to hide a frame-varying pattern behind,
/// so a static camera must produce a bit-identical AO buffer every frame, and the 5x5 denoiser is
/// the only noise-removal stage in the pipeline.
namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied GTAO failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError gtao_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        /// Mirror of `GtaoConstants` in Shaders/gtao_common.slang. All three passes share one
        /// uniform buffer; keeping the layout float4-aligned throughout avoids any std140 padding
        /// disagreement between the two declarations.
        struct GtaoGpuConstants {
            glm::mat4 view{1.0f};
            glm::vec4 viewport_size_pixel_size{};
            glm::vec4 ndc_to_view_mul_add{};
            glm::vec4 radius_falloff_power_thin{};
            glm::vec4 sample_params{};
            glm::vec4 depth_linearize_edge_intensity{};
        };
        static_assert(sizeof(GtaoGpuConstants) == 64 + 5 * 16);

        struct GtaoQualityConfiguration {
            u32 slice_count = 3;
            u32 steps_per_slice = 6;
        };

        /// Slice/step budget per quality level.
        ///
        /// High is the documented default from the paper's practical configuration: 3 slices of 6
        /// steps, 18 taps per pixel. Sample count is deliberately not the primary quality dial -
        /// the spatial denoiser is part of the algorithm, and throwing taps at a noisy result is
        /// both slower and less effective than letting the filter do its job.
        ///
        /// @param quality Quality selector, matching `Engine::AmbientOcclusionQuality`.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] GtaoQualityConfiguration gtao_quality_configuration(u32 quality) noexcept {
            switch (quality) {
                case 0: return GtaoQualityConfiguration{.slice_count = 1, .steps_per_slice = 3};
                case 1: return GtaoQualityConfiguration{.slice_count = 2, .steps_per_slice = 4};
                case 3: return GtaoQualityConfiguration{.slice_count = 4, .steps_per_slice = 8};
                default: return GtaoQualityConfiguration{.slice_count = 3, .steps_per_slice = 6};
            }
        }

        /// Finds the reflected binding with the supplied name, or reports a GTAO error.
        ///
        /// @param resources Reflected resource bindings to search.
        /// @param name Name of the shader resource being resolved.
        /// @param shader_label Label used in the error message when the resource is missing.
        ///
        /// @return Returns the binding index on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<u32> find_gtao_binding(
            const vector<ReflectedResource> &resources, const char *name, const char *shader_label) {
            for (const ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    return resource.binding;
                }
            }
            return unexpected(gtao_error(
                string(shader_label) + " shader reflection is missing the '" + name + "' resource."));
        }

        /// Returns a finite fallback when `value` is not finite.
        ///
        /// @param value `value` value used by the operation.
        /// @param fallback Value substituted when `value` is not finite.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 gtao_finite_or(f32 value, f32 fallback) noexcept {
            return std::isfinite(value) ? value : fallback;
        }

    } // namespace

    /// Finds or creates the XeGTAO compute pipelines (prefilter/main/denoise) and their shared
    /// point-clamp sampler.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_gtao_resources() {
        ZoneScopedN("Renderer::ensure_gtao_resources");
        auto guard = gtao_.lock();
        if (guard->ready) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(gtao_error("Cannot build GTAO resources without an RHI device."));
        }
        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());
        const bool disk_cache = recovery_create_info_.enable_shader_disk_cache;

        auto cleanup = [&]() noexcept {
            const auto destroy_variant = [&](GtaoComputeVariant &variant) noexcept {
                if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                if (variant.module) device->destroy_shader_module(variant.module);
                variant = {};
            };
            destroy_variant(guard->prefilter_depth);
            destroy_variant(guard->main_pass);
            destroy_variant(guard->denoise);
            if (guard->point_sampler) device->destroy_sampler(guard->point_sampler);
            guard->point_sampler = {};
        };

        struct VariantSpec {
            const char *shader_path;
            const char *module_name;
            const char *entry_point;
            const char *label;
            GtaoComputeVariant *variant;
        };
        const array<VariantSpec, 3> specs{
            VariantSpec{"Shaders/gtao_prefilter_depth.slang", "gtao_prefilter_depth", "prefilterDepthMain",
                        "GTAO depth prefilter pipeline", &guard->prefilter_depth},
            VariantSpec{"Shaders/gtao_main.slang", "gtao_main", "gtaoMain",
                        "GTAO main pipeline", &guard->main_pass},
            VariantSpec{"Shaders/gtao_denoise.slang", "gtao_denoise", "gtaoDenoiseMain",
                        "GTAO denoise pipeline", &guard->denoise},
        };

        for (const VariantSpec &spec : specs) {
            GtaoComputeVariant &variant = *spec.variant;

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
                return unexpected(gtao_error(
                    string("compile ") + spec.label + " failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            variant.shader = *shader;

            auto code = variant.shader.entry_point_code(spec.entry_point, shader_target->slang_target.format);
            if (!code) {
                cleanup();
                return unexpected(gtao_error(string("generate ") + spec.label + " bytecode failed: " + code.error().message));
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
                return unexpected(gtao_error(string(spec.label) + " shader reflection produced no bind-group layout."));
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

        // Point + clamp. Linear filtering across a depth discontinuity blends two surfaces into a
        // depth that belongs to neither, and every tap that reads it reconstructs a position
        // floating in empty space; clamping is what keeps a tap that walks off the pyramid from
        // wrapping to the opposite edge of the screen.
        const RHI::SamplerDesc sampler_desc{
            .min_filter = RHI::Filter::Nearest,
            .mag_filter = RHI::Filter::Nearest,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .label = "GTAO depth pyramid sampler",
        };
        auto sampler = device->create_sampler(sampler_desc);
        if (!sampler) {
            cleanup();
            return unexpected(graphics_error_from_rhi(sampler.error(), "create GTAO depth pyramid sampler"));
        }
        guard->point_sampler = *sampler;

        guard->ready = true;
        return {};
    }

    /// Destroys the GTAO pipelines identified by the supplied parameters.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_gtao_resources() noexcept {
        ZoneScopedN("Renderer::destroy_gtao_resources");
        auto guard = gtao_.lock();
        if (RHI::RhiDevice *device = rhi_device()) {
            const auto destroy_variant = [&](GtaoComputeVariant &variant) noexcept {
                if (variant.pipeline) device->destroy_compute_pipeline(variant.pipeline);
                if (variant.pipeline_layout) device->destroy_pipeline_layout(variant.pipeline_layout);
                if (variant.bind_group_layout) device->destroy_bind_group_layout(variant.bind_group_layout);
                if (variant.module) device->destroy_shader_module(variant.module);
            };
            destroy_variant(guard->prefilter_depth);
            destroy_variant(guard->main_pass);
            destroy_variant(guard->denoise);
            if (guard->point_sampler) device->destroy_sampler(guard->point_sampler);
        }
        *guard = GtaoResources{};
    }

    /// Finds or creates the per-window GTAO linear-depth pyramid for `render_extent`, reallocating
    /// it when the render extent changes.
    ///
    /// @param pyramid Per-window pyramid to size.
    /// @param render_extent Full-resolution render extent the AO runs at.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_gtao_depth_pyramid(GtaoDepthPyramid &pyramid,
                                                              Core::Extent2D render_extent) {
        ZoneScopedN("Renderer::ensure_gtao_depth_pyramid");
        if (Core::is_zero(render_extent)) {
            return unexpected(gtao_error("Cannot build a GTAO depth pyramid for a zero-sized render extent."));
        }
        const bool matches = pyramid.extent == render_extent && pyramid.texture && pyramid.full_view &&
            pyramid.mip_views.size() == kGtaoDepthMipLevels;
        if (matches) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(gtao_error("Cannot build a GTAO depth pyramid without an RHI device."));
        }
        destroy_gtao_depth_pyramid(pyramid);
        pyramid.extent = render_extent;

        // R32Float rather than the R16Float XeGTAO prefers. FP16 linear depth is enough precision
        // for the horizon search and would halve this pass's bandwidth, but r16f storage images
        // require Vulkan's shaderStorageImageExtendedFormats, which this engine does not negotiate
        // (and the RHI exposes no per-format storage capability query to branch on). R32Float is in
        // the mandatory storage-image set on every backend, so it is the portable correct choice;
        // revisit if the RHI grows a format-capability query.
        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = render_extent.x, .height = render_extent.y, .depth_or_layers = 1},
            .mip_levels = kGtaoDepthMipLevels,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "GTAO linear depth pyramid",
        });
        if (!texture) {
            destroy_gtao_depth_pyramid(pyramid);
            return unexpected(graphics_error_from_rhi(texture.error(), "create GTAO linear depth pyramid"));
        }
        pyramid.texture = *texture;

        for (u32 level = 0; level < kGtaoDepthMipLevels; ++level) {
            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = level,
                .mip_level_count = 1,
                .label = "GTAO depth pyramid mip view",
            });
            if (!view) {
                destroy_gtao_depth_pyramid(pyramid);
                return unexpected(graphics_error_from_rhi(view.error(), "create GTAO depth pyramid mip view"));
            }
            pyramid.mip_views.push_back(*view);
        }

        auto full_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .base_mip_level = 0,
            .mip_level_count = RHI::all_remaining,
            .label = "GTAO depth pyramid full view",
        });
        if (!full_view) {
            destroy_gtao_depth_pyramid(pyramid);
            return unexpected(graphics_error_from_rhi(full_view.error(), "create GTAO depth pyramid full view"));
        }
        pyramid.full_view = *full_view;
        return {};
    }

    /// Destroys the GTAO depth pyramid identified by the supplied parameters.
    ///
    /// @param pyramid `pyramid` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_gtao_depth_pyramid(GtaoDepthPyramid &pyramid) noexcept {
        ZoneScopedN("Renderer::destroy_gtao_depth_pyramid");
        if (RHI::RhiDevice *device = rhi_device()) {
            if (pyramid.full_view) device->destroy_texture_view(pyramid.full_view);
            for (RHI::TextureViewHandle view : pyramid.mip_views) {
                if (view) device->destroy_texture_view(view);
            }
            if (pyramid.texture) device->destroy_texture(pyramid.texture);
        }
        pyramid = GtaoDepthPyramid{};
    }

    /// Records the GTAO depth prefilter pass: hardware depth to linear view depth plus the
    /// five-level depth-aware mip chain.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_gtao_prefilter_depth(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle scene_depth_view,
        const GtaoDepthPyramid &pyramid,
        RHI::BufferHandle constants_buffer,
        Core::Extent2D render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_gtao_prefilter_depth");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !scene_depth_view || !constants_buffer ||
            pyramid.mip_views.size() != kGtaoDepthMipLevels) {
            return unexpected(gtao_error("Cannot record the GTAO depth prefilter without valid depth/pyramid/constants."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        {
            auto guard = gtao_.lock();
            layout = guard->prefilter_depth.bind_group_layout;
            pipeline = guard->prefilter_depth.pipeline;
            resources = guard->prefilter_depth.resources;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind = [&](const char *name, RHI::TextureViewHandle view, RHI::BufferHandle buffer) -> Core::RendererResult {
            auto binding = find_gtao_binding(resources, name, "GTAO depth prefilter");
            if (!binding) return unexpected(binding.error());
            entries.push_back(RHI::BindGroupEntry{.binding = *binding, .buffer = buffer, .texture_view = view});
            return {};
        };
        if (auto r = bind("gtao", {}, constants_buffer); !r.has_value()) return r;
        if (auto r = bind("sceneDepth", scene_depth_view, {}); !r.has_value()) return r;
        static constexpr array<const char *, 5> mip_names{
            "viewDepthMip0", "viewDepthMip1", "viewDepthMip2", "viewDepthMip3", "viewDepthMip4"};
        for (u32 level = 0; level < kGtaoDepthMipLevels; ++level) {
            if (auto r = bind(mip_names[level], pyramid.mip_views[level], {}); !r.has_value()) return r;
        }

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "GTAO depth prefilter bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create GTAO depth prefilter bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        // One 8x8 group produces a 16x16 block of mip 0 (two mip-0 texels per thread per axis),
        // which is exactly the footprint needed to reduce down to a single mip-4 texel in shared
        // memory without a second dispatch.
        pass.dispatch((render_extent.x + 15u) / 16u, (render_extent.y + 15u) / 16u, 1);
        return {};
    }

    /// Records the GTAO horizon-search pass, writing the raw AO term and the depth-edge information
    /// the denoiser consumes.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_gtao_main(
        RHI::ComputePassEncoder &pass,
        const GtaoDepthPyramid &pyramid,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle raw_ao_view,
        RHI::TextureViewHandle edges_view,
        RHI::BufferHandle constants_buffer,
        Core::Extent2D render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_gtao_main");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !pyramid.full_view || !gbuffer_normal_view || !raw_ao_view || !edges_view ||
            !constants_buffer) {
            return unexpected(gtao_error("Cannot record the GTAO main pass without valid views/constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        RHI::SamplerHandle point_sampler{};
        vector<ReflectedResource> resources;
        {
            auto guard = gtao_.lock();
            layout = guard->main_pass.bind_group_layout;
            pipeline = guard->main_pass.pipeline;
            point_sampler = guard->point_sampler;
            resources = guard->main_pass.resources;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind = [&](const char *name, const RHI::BindGroupEntry &prototype) -> Core::RendererResult {
            auto binding = find_gtao_binding(resources, name, "GTAO main");
            if (!binding) return unexpected(binding.error());
            RHI::BindGroupEntry entry = prototype;
            entry.binding = *binding;
            entries.push_back(entry);
            return {};
        };
        if (auto r = bind("gtao", RHI::BindGroupEntry{.buffer = constants_buffer}); !r.has_value()) return r;
        if (auto r = bind("viewDepthPyramid", RHI::BindGroupEntry{.texture_view = pyramid.full_view}); !r.has_value()) return r;
        if (auto r = bind("gbufferNormal", RHI::BindGroupEntry{.texture_view = gbuffer_normal_view}); !r.has_value()) return r;
        if (auto r = bind("gtaoPointSampler", RHI::BindGroupEntry{.sampler = point_sampler}); !r.has_value()) return r;
        if (auto r = bind("ambientOcclusionOut", RHI::BindGroupEntry{.texture_view = raw_ao_view}); !r.has_value()) return r;
        if (auto r = bind("edgesOut", RHI::BindGroupEntry{.texture_view = edges_view}); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "GTAO main bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create GTAO main bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Records the 5x5 edge-aware GTAO spatial denoise pass.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_gtao_denoise(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle raw_ao_view,
        RHI::TextureViewHandle edges_view,
        RHI::TextureViewHandle gbuffer_normal_view,
        RHI::TextureViewHandle output_view,
        RHI::BufferHandle constants_buffer,
        Core::Extent2D render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_gtao_denoise");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !raw_ao_view || !edges_view || !gbuffer_normal_view || !output_view ||
            !constants_buffer) {
            return unexpected(gtao_error("Cannot record the GTAO denoise pass without valid views/constants/device."));
        }

        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        vector<ReflectedResource> resources;
        {
            auto guard = gtao_.lock();
            layout = guard->denoise.bind_group_layout;
            pipeline = guard->denoise.pipeline;
            resources = guard->denoise.resources;
        }

        vector<RHI::BindGroupEntry> entries;
        const auto bind = [&](const char *name, const RHI::BindGroupEntry &prototype) -> Core::RendererResult {
            auto binding = find_gtao_binding(resources, name, "GTAO denoise");
            if (!binding) return unexpected(binding.error());
            RHI::BindGroupEntry entry = prototype;
            entry.binding = *binding;
            entries.push_back(entry);
            return {};
        };
        if (auto r = bind("gtao", RHI::BindGroupEntry{.buffer = constants_buffer}); !r.has_value()) return r;
        if (auto r = bind("rawAmbientOcclusion", RHI::BindGroupEntry{.texture_view = raw_ao_view}); !r.has_value()) return r;
        if (auto r = bind("gtaoEdges", RHI::BindGroupEntry{.texture_view = edges_view}); !r.has_value()) return r;
        if (auto r = bind("gbufferNormal", RHI::BindGroupEntry{.texture_view = gbuffer_normal_view}); !r.has_value()) return r;
        if (auto r = bind("ambientOcclusionOut", RHI::BindGroupEntry{.texture_view = output_view}); !r.has_value()) return r;

        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "GTAO denoise bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create GTAO denoise bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Builds the GTAO render-graph module, returning the screen-space ambient-occlusion texture the
    /// deferred lighting pass modulates indirect diffuse with. When AO is disabled (or the view
    /// cannot support it) this imports a 1x1 white dummy so the lighting bind group's layout is
    /// identical either way.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RenderGraphTextureHandle> Renderer::build_gtao_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &slot,
        RenderGraphTextureHandle gbuffer_normal,
        RenderGraphTextureHandle depth_texture) {
        ZoneScopedN("Renderer::build_gtao_module");
        // Per frame in flight: this slot's fence was waited before recording began, so the pyramid
        // is idle and cannot be aliased by another frame still executing on the GPU.
        GtaoDepthPyramid &pyramid = slot.gtao_depth_pyramid;
        const RenderGraphSettings &settings = submission.render_graph;
        const Core::Extent2D render_extent = context.render_extent;

        // The spectral ray-traced AO integrator resolves the same visibility term against real
        // geometry; running the screen-space approximation alongside it would only burn bandwidth on
        // a result the lighting pass discards.
        const bool ray_traced_ambient_occlusion =
            settings.spectral_path_tracing.mode == SpectralRenderMode::AmbientOcclusionOnly;
        // Orthographic views are excluded on purpose. The horizon search's screen-to-view mapping
        // (gtaoViewSpacePosition) assumes a perspective divide; running it under an orthographic
        // projection would not fail loudly, it would quietly scale every radius by depth and
        // produce AO that grows with distance.
        const bool orthographic = std::abs(submission.camera.projection[3][3]) > 0.5f;
        // Below the minimum extent the depth pyramid cannot supply all five mip levels.
        const bool too_small = std::max(render_extent.x, render_extent.y) < kGtaoMinimumRenderExtent;
        if (!settings.ambient_occlusion || ray_traced_ambient_occlusion || orthographic || too_small) {
            auto default_texture = ensure_default_white_texture();
            if (!default_texture) return unexpected(default_texture.error());
            const TextureResource *white = texture(*default_texture);
            if (white == nullptr || !white->texture || !white->view) {
                return unexpected(gtao_error("GTAO fallback dummy texture is missing its RHI resources."));
            }
            return context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = white->texture,
                .default_view = white->view,
                .format = RHI::Format::RGBA8Unorm,
                .extent = RHI::Extent3D{.width = white->width, .height = white->height, .depth_or_layers = 1},
                .usage = RHI::TextureUsage::Sampled,
                .label = "GTAO disabled dummy ambient occlusion",
            });
        }

        if (Core::RendererResult ready = ensure_gtao_resources(); !ready.has_value()) {
            return unexpected(ready.error());
        }
        if (Core::RendererResult ready = ensure_gtao_depth_pyramid(pyramid, render_extent); !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(gtao_error("Cannot build GTAO without an RHI device."));
        }

        const glm::mat4 &projection = submission.camera.projection;
        const f32 focal_x = std::abs(projection[0][0]) > 1.0e-6f ? projection[0][0] : 1.0f;
        const f32 focal_y = std::abs(projection[1][1]) > 1.0e-6f ? projection[1][1] : 1.0f;
        const f32 near_plane = std::max(gtao_finite_or(submission.camera.near_plane, 0.01f), 1.0e-4f);
        const f32 far_plane = std::max(gtao_finite_or(submission.camera.far_plane, 1000.0f), near_plane * 1.001f);
        const f32 width = static_cast<f32>(std::max(render_extent.x, 1u));
        const f32 height = static_cast<f32>(std::max(render_extent.y, 1u));

        const GtaoQualityConfiguration quality = gtao_quality_configuration(settings.ambient_occlusion_quality);
        const f32 radius = std::max(gtao_finite_or(settings.ambient_occlusion_radius, 1.0f), 1.0e-3f);
        const f32 falloff_range = std::clamp(gtao_finite_or(settings.ambient_occlusion_falloff_range, 0.615f), 0.05f, 1.0f);
        const f32 final_value_power = std::clamp(gtao_finite_or(settings.ambient_occlusion_final_value_power, 2.2f), 0.5f, 5.0f);
        const f32 thin_occluder = std::clamp(
            gtao_finite_or(settings.ambient_occlusion_thin_occluder_compensation, 0.0f), 0.0f, 0.7f);
        const f32 distribution_power = std::clamp(
            gtao_finite_or(settings.ambient_occlusion_sample_distribution_power, 2.0f), 1.0f, 3.0f);
        const f32 intensity = std::clamp(gtao_finite_or(settings.ambient_occlusion_intensity, 1.0f), 0.0f, 1.0f);

        const GtaoGpuConstants constants{
            .view = submission.camera.view,
            .viewport_size_pixel_size = glm::vec4{width, height, 1.0f / width, 1.0f / height},
            // Screen UV -> view-space ray scale. The y lanes are negated relative to x because the
            // engine's screen UV runs top-down while view-space Y runs up (see uvToNdc in
            // Shaders/sturdy_common.slang) - reimplementing that flip inline is exactly how it gets
            // silently dropped, so it is derived here once, from the projection actually in use.
            .ndc_to_view_mul_add = glm::vec4{2.0f / focal_x, -2.0f / focal_y, -1.0f / focal_x, 1.0f / focal_y},
            .radius_falloff_power_thin = glm::vec4{radius, falloff_range, final_value_power, thin_occluder},
            // .y is XeGTAO's DepthMIPSamplingOffset: how far a tap must travel before it drops to a
            // coarser depth mip. Fixed rather than exposed - it trades cache behaviour against
            // horizon accuracy, and 3.3 is the value XeGTAO tuned for full-resolution AO.
            .sample_params = glm::vec4{distribution_power, 3.3f, static_cast<f32>(quality.slice_count),
                                       static_cast<f32>(quality.steps_per_slice)},
            .depth_linearize_edge_intensity = glm::vec4{
                near_plane * far_plane / (far_plane - near_plane),
                far_plane / (far_plane - near_plane),
                // Screen-border fade width. Taps inside this band are attenuated toward the
                // hemisphere horizon instead of clamping to the border texel, which is what keeps
                // grazing views of large flat surfaces from growing a dark stripe down the edge.
                std::max(4.0f, 0.01f * std::min(width, height)),
                intensity,
            },
        };

        auto constant_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(constants),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "GTAO frame constants",
        });
        if (!constant_buffer) {
            return unexpected(graphics_error_from_rhi(constant_buffer.error(), "create GTAO frame constants"));
        }
        if (auto written = device->write_buffer(*constant_buffer, 0, std::as_bytes(span{&constants, 1})); !written) {
            device->destroy_buffer(*constant_buffer);
            return unexpected(graphics_error_from_rhi(written.error(), "write GTAO frame constants"));
        }
        slot.transient_buffers.push_back(*constant_buffer);
        const RHI::BufferHandle constants_buffer = *constant_buffer;

        const RenderGraphTextureHandle depth_pyramid = context.graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = pyramid.texture,
            .default_view = pyramid.full_view,
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = render_extent.x, .height = render_extent.y, .depth_or_layers = 1},
            .mip_levels = kGtaoDepthMipLevels,
            .usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled,
            .label = "GTAO linear depth pyramid",
        });

        // Raw AO and edges are separate targets rather than one packed one: both are mandatory
        // storage-image formats on every backend, and splitting them lets the denoiser read the AO
        // term and its edge mask through independent cache lines.
        const RenderGraphTextureHandle raw_ambient_occlusion = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::R32Float,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "GTAO raw ambient occlusion",
        });
        const RenderGraphTextureHandle edges = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA8Unorm,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "GTAO depth edges",
        });

        context.graph.add_compute_pass("gtao prefilter depth"_ustr)
            .add_sampled_texture(depth_texture)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = depth_pyramid, .read = false, .write = true})
            .set_execute([this, &submission, &slot, depth_texture, constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_gtao_prefilter_depth(
                    graph_context.compute_pass(),
                    graph_context.texture(depth_texture).default_view,
                    slot.gtao_depth_pyramid, constants_buffer, render_extent,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("gtao main"_ustr)
            .add_sampled_texture(depth_pyramid)
            .add_sampled_texture(gbuffer_normal)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = raw_ambient_occlusion, .read = false, .write = true})
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = edges, .read = false, .write = true})
            .set_execute([this, &submission, &slot, gbuffer_normal, raw_ambient_occlusion, edges,
                          constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_gtao_main(
                    graph_context.compute_pass(), slot.gtao_depth_pyramid,
                    graph_context.texture(gbuffer_normal).default_view,
                    graph_context.texture(raw_ambient_occlusion).default_view,
                    graph_context.texture(edges).default_view,
                    constants_buffer, render_extent, submission.transient_bind_groups);
            });

        if (!settings.ambient_occlusion_denoise) {
            // Validation path only: the raw buffer is what the lighting pass will consume, which is
            // the intended way to see how much of the final result the denoiser is responsible for.
            return raw_ambient_occlusion;
        }

        const RenderGraphTextureHandle denoised = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::R32Float,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "GTAO ambient occlusion",
        });
        context.graph.add_compute_pass("gtao denoise"_ustr)
            .add_sampled_texture(raw_ambient_occlusion)
            .add_sampled_texture(edges)
            .add_sampled_texture(gbuffer_normal)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = denoised, .read = false, .write = true})
            .set_execute([this, &submission, raw_ambient_occlusion, edges, gbuffer_normal, denoised,
                          constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_gtao_denoise(
                    graph_context.compute_pass(),
                    graph_context.texture(raw_ambient_occlusion).default_view,
                    graph_context.texture(edges).default_view,
                    graph_context.texture(gbuffer_normal).default_view,
                    graph_context.texture(denoised).default_view,
                    constants_buffer, render_extent, submission.transient_bind_groups);
            });

        return denoised;
    }

} // namespace SFT::Renderer
