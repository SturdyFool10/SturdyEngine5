#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::string;
using std::unexpected;

namespace SFT::Renderer {
    namespace {
        /// Creates an error result describing the supplied offscreen target failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError offscreen_target_error(string message) {
            return Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::OperationFailed,
                std::move(message),
            };
        }

        /// Push constants of `Shaders/offscreen_target_finalize.slang` (`FinalizeConstants`). 16 bytes: the same layout as a
        /// push-constant block and as WebGPU's std140 uniform stand-in.
        struct FinalizeConstants {
            u32 decode_source = 0;
            u32 encode_output = 0;
            u32 padding0 = 0;
            u32 padding1 = 0;
        };
        static_assert(sizeof(FinalizeConstants) == 16);

        [[nodiscard]] CustomPostProcessEffect finalize_effect(const char *entry_point, FinalizeConstants constants, const char *label) {
            CustomPostProcessEffect effect;
            effect.shader_path = "Shaders/offscreen_target_finalize.slang";
            effect.module_name = "offscreen_target_finalize";
            effect.fragment_entry_point = entry_point;
            const std::span<const std::byte> bytes = std::as_bytes(std::span<const FinalizeConstants>{&constants, 1});
            effect.push_constants.assign(bytes.begin(), bytes.end());
            effect.label = UString{std::string_view{label}};
            return effect;
        }

        [[nodiscard]] Core::Extent2D mip_extent(Core::Extent2D extent, u32 level) noexcept {
            return Core::Extent2D{std::max(extent.x >> level, 1u), std::max(extent.y >> level, 1u)};
        }

        void set_full_viewport(RHI::RenderPassEncoder &pass, Core::Extent2D extent) {
            pass.set_viewport(RHI::Viewport{
                .x = 0.0f,
                .y = 0.0f,
                .width = static_cast<f32>(extent.x),
                .height = static_cast<f32>(extent.y),
                .min_depth = 0.0f,
                .max_depth = 1.0f,
            });
            pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y});
        }
    } // namespace

    Core::RendererExpected<ResolvedOffscreenRenderTargetConfig> Renderer::resolve_offscreen_render_target_on_device(
        const OffscreenRenderTargetDescription &request) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(offscreen_target_error("Cannot create an off-screen render target without an RHI device."));
        }
        const OffscreenTargetDeviceCapabilities capabilities{
            .limits = device->limits(),
            .format_capabilities = [device](RHI::Format format) { return device->format_capabilities(format); },
        };
        auto config = ::SFT::Renderer::resolve_offscreen_render_target(request, capabilities);
        if (!config) {
            return unexpected(offscreen_target_error(std::move(config.error())));
        }
        for (const string &adjustment : config->adjustments) {
            Foundation::log_warn("Off-screen render target '{}': {}", request.label.cpp_string_view(), adjustment);
        }
        return std::move(*config);
    }

    Core::RendererExpected<RHI::SamplerHandle> Renderer::create_offscreen_render_target_sampler(
        const ResolvedOffscreenRenderTargetConfig &config, const UString &label) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(offscreen_target_error("Cannot create an off-screen render target sampler without an RHI device."));
        }
        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = config.filter,
            .mag_filter = config.filter,
            // Trilinear across a mip chain; a single level has nothing to blend.
            .mipmap_mode = config.mip_levels > 1 && config.filter == RHI::Filter::Linear ? RHI::MipmapMode::Linear : RHI::MipmapMode::Nearest,
            .address_u = config.address,
            .address_v = config.address,
            .address_w = config.address,
            .max_lod = static_cast<f32>(config.mip_levels - 1),
            .max_anisotropy = config.max_anisotropy,
            .label = label.empty() ? "renderer off-screen target" : label.c_str(),
        });
        if (!sampler) {
            return unexpected(graphics_error_from_rhi(sampler.error(), "create off-screen render target sampler"));
        }
        return *sampler;
    }

    void Renderer::publish_offscreen_render_target(OffscreenRenderTargetRecord &target, TextureResource &sampled) noexcept {
        sampled.texture = target.texture;
        sampled.view = target.view;
        sampled.sampler = target.sampler;
        sampled.width = target.config.extent.x;
        sampled.height = target.config.extent.y;
        sampled.mip_levels = static_cast<u32>(std::max<usize>(target.mip_views.size(), 1));
        sampled.format = target.config.format;
        sampled.nearest_sampling = target.config.filter == RHI::Filter::Nearest;
        sampled.owns_gpu_resources = false;
        sampled.externally_destroyable = false;
    }

    void Renderer::destroy_offscreen_render_target_gpu_resources(const OffscreenRenderTargetGpuResources &gpu) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        if (gpu.sampler) device->destroy_sampler(gpu.sampler);
        for (const RHI::TextureViewHandle view : gpu.mip_views) {
            if (view) device->destroy_texture_view(view);
        }
        if (gpu.view) device->destroy_texture_view(gpu.view);
        if (gpu.texture) device->destroy_texture(gpu.texture);
    }

    void Renderer::destroy_offscreen_render_target_gpu_objects(OffscreenRenderTargetRecord &target) noexcept {
        destroy_offscreen_render_target_gpu_resources(OffscreenRenderTargetGpuResources{
            .texture = target.texture,
            .view = target.view,
            .mip_views = target.mip_views,
            .sampler = target.sampler,
        });
        target.texture = {};
        target.view = {};
        target.mip_views.clear();
        target.sampler = {};
    }

    /// Creates a offscreen render target GPU resources from the supplied parameters.
    ///
    /// @param description Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<Renderer::OffscreenRenderTargetGpuResources>
    Renderer::create_offscreen_render_target_gpu_resources(
        const ResolvedOffscreenRenderTargetConfig &config, const UString &target_label) {
        ZoneScopedN("Renderer::create_offscreen_render_target_gpu_resources");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(offscreen_target_error(
                "Cannot create an off-screen render target without an RHI device."));
        }
        const char *label = target_label.empty()
            ? "renderer off-screen target"
            : target_label.c_str();
        const u32 mips = config.mip_levels;

        RHI::TextureUsage usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc;
        if (config.storage) {
            usage = usage | RHI::TextureUsage::Storage;
        }
        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = config.format,
            .extent = RHI::Extent3D{
                .width = config.extent.x,
                .height = config.extent.y,
                .depth_or_layers = 1,
            },
            .mip_levels = mips,
            .samples = RHI::SampleCount::X1,
            .usage = usage,
            .label = label,
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create off-screen render target"));
        }
        OffscreenRenderTargetGpuResources gpu{.texture = *texture};

        auto view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .base_mip_level = 0,
            .mip_level_count = mips,
            .label = label,
        });
        if (!view) {
            destroy_offscreen_render_target_gpu_resources(gpu);
            return unexpected(graphics_error_from_rhi(view.error(), "create off-screen render target view"));
        }
        gpu.view = *view;
        for (u32 level = 0; level < mips; ++level) {
            auto level_view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = level,
                .mip_level_count = 1,
                .label = label,
            });
            if (!level_view) {
                destroy_offscreen_render_target_gpu_resources(gpu);
                return unexpected(graphics_error_from_rhi(level_view.error(), "create off-screen render target mip view"));
            }
            gpu.mip_views.push_back(*level_view);
        }

        auto sampler = create_offscreen_render_target_sampler(config, target_label);
        if (!sampler) {
            destroy_offscreen_render_target_gpu_resources(gpu);
            return unexpected(sampler.error());
        }
        gpu.sampler = *sampler;
        return gpu;
    }

    /// Creates a offscreen render target from the supplied parameters.
    ///
    /// @param description Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<OffscreenRenderTargetHandle> Renderer::create_offscreen_render_target(
        const OffscreenRenderTargetDescription &description) {
        ZoneScopedN("Renderer::create_offscreen_render_target");
        auto config = resolve_offscreen_render_target_on_device(description);
        if (!config) {
            return unexpected(config.error());
        }
        auto gpu = create_offscreen_render_target_gpu_resources(*config, description.label);
        if (!gpu) {
            return unexpected(gpu.error());
        }

        const char *label = description.label.empty()
            ? "renderer off-screen target sampling view"
            : description.label.c_str();
        auto sampled_texture = adopt_texture(gpu->texture, gpu->view, gpu->sampler, label);
        if (!sampled_texture) {
            destroy_offscreen_render_target_gpu_resources(*gpu);
            return unexpected(sampled_texture.error());
        }
        TextureResource *sampled_resource = texture(*sampled_texture);
        if (sampled_resource == nullptr) {
            destroy_texture(*sampled_texture);
            destroy_offscreen_render_target_gpu_resources(*gpu);
            return unexpected(offscreen_target_error(
                "Failed to retain the off-screen target's sampling wrapper."));
        }
        auto targets = offscreen_render_targets_.lock();
        const OffscreenRenderTargetHandle handle{static_cast<u64>(targets->size() + 1)};
        targets->push_back(OffscreenRenderTargetRecord{
            .description = description,
            .config = std::move(*config),
            .texture = gpu->texture,
            .view = gpu->view,
            .mip_views = gpu->mip_views,
            .sampler = gpu->sampler,
            .sampled_texture = *sampled_texture,
            .initialized = false,
            .alive = true,
        });
        // Size, format, levels and sampling are what a consumer of the sampling wrapper reads (a texture's format and extent, the
        // sampler a material binds); keep them in step with the target.
        publish_offscreen_render_target(targets->back(), *sampled_resource);
        return handle;
    }

    Core::RendererResult Renderer::reconfigure_offscreen_render_target(OffscreenRenderTargetHandle handle,
                                                                       const OffscreenRenderTargetDescription &description) {
        ZoneScopedN("Renderer::reconfigure_offscreen_render_target");
        if (!handle) {
            return unexpected(offscreen_target_error("Cannot reconfigure an invalid off-screen render target."));
        }
        auto config = resolve_offscreen_render_target_on_device(description);
        if (!config) {
            return unexpected(config.error());
        }
        const auto same_sampling = [](const ResolvedOffscreenRenderTargetConfig &a, const ResolvedOffscreenRenderTargetConfig &b) {
            return a.filter == b.filter && a.address == b.address && a.max_anisotropy == b.max_anisotropy;
        };
        {
            auto targets = offscreen_render_targets_.lock();
            if (handle.value > targets->size() || !(*targets)[static_cast<usize>(handle.value - 1)].alive) {
                return unexpected(offscreen_target_error("Cannot reconfigure an unknown or destroyed off-screen render target."));
            }
            OffscreenRenderTargetRecord &current = (*targets)[static_cast<usize>(handle.value - 1)];
            // Nothing the GPU objects depend on changed (a label, an MSAA preference, an Auto that resolves the same): no wait.
            if (current.config.same_image_as(*config) && same_sampling(current.config, *config)) {
                current.description = description;
                current.config = std::move(*config);
                return {};
            }
        }

        // The old objects may still be referenced by frames in flight; settle them before replacing anything.
        wait_idle();

        auto targets = offscreen_render_targets_.lock();
        OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        if (!target.alive) {
            return unexpected(offscreen_target_error("Cannot reconfigure a destroyed off-screen render target."));
        }
        if (!target.sampled_texture || target.sampled_texture.value > textures_.size()) {
            return unexpected(offscreen_target_error("Cannot reconfigure an off-screen target whose sampling wrapper is missing."));
        }
        TextureResource &sampled = textures_[static_cast<usize>(target.sampled_texture.value - 1)];

        if (!target.config.same_image_as(*config)) {
            auto gpu = create_offscreen_render_target_gpu_resources(*config, description.label);
            if (!gpu) {
                return unexpected(gpu.error());
            }
            destroy_offscreen_render_target_gpu_objects(target);
            target.texture = gpu->texture;
            target.view = gpu->view;
            target.mip_views = gpu->mip_views;
            target.sampler = gpu->sampler;
            // A new image has no contents; the next frame aimed at it clears and redraws it.
            target.initialized = false;
        } else {
            // Only how it is read changed: swap the sampler and keep the pixels.
            auto sampler = create_offscreen_render_target_sampler(*config, description.label);
            if (!sampler) {
                return unexpected(sampler.error());
            }
            if (RHI::RhiDevice *device = rhi_device(); device != nullptr && target.sampler) {
                device->destroy_sampler(target.sampler);
            }
            target.sampler = *sampler;
        }
        target.description = description;
        target.config = std::move(*config);
        // Everything holding the sampling wrapper's handle (UI images, materials) now sees the new GPU objects.
        publish_offscreen_render_target(target, sampled);
        return {};
    }

    /// Destroys the offscreen render target identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    Core::RendererResult Renderer::resize_offscreen_render_target(OffscreenRenderTargetHandle handle,
                                                                  Core::Extent2D extent) {
        ZoneScopedN("Renderer::resize_offscreen_render_target");
        if (!handle) {
            return unexpected(offscreen_target_error("Cannot resize an invalid off-screen render target."));
        }
        optional<OffscreenRenderTargetDescription> resized = offscreen_render_target_description(handle);
        if (!resized) {
            return unexpected(offscreen_target_error("Cannot resize an unknown or destroyed off-screen render target."));
        }
        resized->extent = extent;
        return reconfigure_offscreen_render_target(handle, *resized);
    }

    void Renderer::destroy_offscreen_render_target(OffscreenRenderTargetHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_offscreen_render_target");
        if (!handle) {
            return;
        }


        wait_idle();

        auto targets = offscreen_render_targets_.lock();
        if (handle.value > targets->size()) {
            return;
        }
        OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        if (!target.alive) {
            return;
        }
        if (target.sampled_texture && target.sampled_texture.value <= textures_.size()) {
            TextureResource &sampled = textures_[static_cast<usize>(target.sampled_texture.value - 1)];
            if (sampled.handle == target.sampled_texture) {
                sampled = {};
            }
        }
        destroy_offscreen_render_target_gpu_objects(target);
        target = {};
    }

    /// Performs the offscreen render target description operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<OffscreenRenderTargetDescription> Renderer::offscreen_render_target_description(
        OffscreenRenderTargetHandle handle) const {
        ZoneScopedN("Renderer::offscreen_render_target_description");
        auto targets = offscreen_render_targets_.lock();
        if (!handle || handle.value > targets->size()) {
            return std::nullopt;
        }
        const OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        return target.alive ? optional<OffscreenRenderTargetDescription>{target.description} : std::nullopt;
    }

    optional<ResolvedOffscreenRenderTargetConfig> Renderer::offscreen_render_target_config(OffscreenRenderTargetHandle handle) const {
        auto targets = offscreen_render_targets_.lock();
        if (!handle || handle.value > targets->size()) {
            return std::nullopt;
        }
        const OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        return target.alive ? optional<ResolvedOffscreenRenderTargetConfig>{target.config} : std::nullopt;
    }

    /// Performs the offscreen render target texture operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    TextureHandle Renderer::offscreen_render_target_texture(OffscreenRenderTargetHandle handle) const noexcept {
        ZoneScopedN("Renderer::offscreen_render_target_texture");
        auto targets = offscreen_render_targets_.lock();
        if (!handle || handle.value > targets->size()) {
            return {};
        }
        const OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        return target.alive ? target.sampled_texture : TextureHandle{};
    }

    /// Resolves offscreen render target into the concrete value used by the engine.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<Renderer::ResolvedOffscreenRenderTarget> Renderer::resolve_offscreen_render_target(
        OffscreenRenderTargetHandle handle) const noexcept {
        ZoneScopedN("Renderer::resolve_offscreen_render_target");
        auto targets = offscreen_render_targets_.lock();
        if (!handle || handle.value > targets->size()) {
            return std::nullopt;
        }
        const OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        if (!target.alive || !target.texture || target.mip_views.empty() || Core::is_zero(target.config.extent)) {
            return std::nullopt;
        }
        return ResolvedOffscreenRenderTarget{
            .texture = target.texture,
            .view = target.mip_views.front(),
            .mip_views = target.mip_views,
            .extent = target.config.extent,
            .format = target.config.format,
            .msaa_samples = target.config.msaa_samples,
            .storage = target.config.storage,
            .initialized = target.initialized,
        };
    }

    /// Marks offscreen render target initialized using the supplied arguments and current state.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::mark_offscreen_render_target_initialized(OffscreenRenderTargetHandle handle) noexcept {
        ZoneScopedN("Renderer::mark_offscreen_render_target_initialized");
        auto targets = offscreen_render_targets_.lock();
        if (!handle || handle.value > targets->size()) {
            return;
        }
        OffscreenRenderTargetRecord &target = (*targets)[static_cast<usize>(handle.value - 1)];
        if (target.alive) {
            target.initialized = true;
        }
    }

    Core::RendererResult Renderer::add_offscreen_target_finalize_passes(RenderGraph &graph, RenderGraphTextureHandle target,
                                                                        RenderGraphTextureHandle linear_source,
                                                                        const ResolvedOffscreenRenderTarget &resolved,
                                                                        std::vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::add_offscreen_target_finalize_passes");
        const RHI::Format format = resolved.format;
        const bool shader_encoded = offscreen_render_target_format_needs_shader_encode(format);

        if (linear_source) {
            // The frame drew linear light into a half-float intermediate; encode it into level 0 the way an *Srgb format would have.
            CustomPostProcessEffect effect = finalize_effect("encodeMain", FinalizeConstants{.encode_output = 1u}, "off-screen target encode");
            if (Core::RendererResult ready = prepare_fullscreen_effect(effect, format); !ready.has_value()) {
                return ready;
            }
            const Core::Extent2D extent = resolved.extent;
            graph.add_render_pass("off-screen target encode"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = target,
                    .view = resolved.mip_views.front(),
                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = 0, .mip_level_count = 1},
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = linear_source})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
                .set_execute([this, &transient_bind_groups, linear_source, format, extent,
                              effect = std::move(effect)](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    set_full_viewport(pass, extent);
                    return record_fullscreen_effect(pass, context.texture(linear_source).default_view, format, effect, transient_bind_groups);
                });
        }

        // Each level is a 2x2 box filter of the one above. Encoded data (a shader-encoded format) is decoded first so the average is
        // taken in linear light; an *Srgb format decodes on read and encodes on write by itself.
        const FinalizeConstants downsample_constants{.decode_source = shader_encoded ? 1u : 0u, .encode_output = shader_encoded ? 1u : 0u};
        for (u32 level = 1; level < resolved.mip_views.size(); ++level) {
            CustomPostProcessEffect effect = finalize_effect("downsampleMain", downsample_constants, "off-screen target mip");
            if (Core::RendererResult ready = prepare_fullscreen_effect(effect, format); !ready.has_value()) {
                return ready;
            }
            const Core::Extent2D extent = mip_extent(resolved.extent, level);
            const RHI::TextureViewHandle source_view = resolved.mip_views[level - 1];
            graph.add_render_pass("off-screen target mip"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = target,
                    .view = resolved.mip_views[level],
                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = level, .mip_level_count = 1},
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                    .texture = target,
                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = level - 1, .mip_level_count = 1},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.x, .height = extent.y})
                .set_execute([this, &transient_bind_groups, source_view, format, extent,
                              effect = std::move(effect)](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    set_full_viewport(pass, extent);
                    return record_fullscreen_effect(pass, source_view, format, effect, transient_bind_groups);
                });
        }
        return {};
    }

    Core::RendererResult Renderer::dispatch_into_offscreen_render_target(OffscreenRenderTargetHandle handle, ComputeKernelId kernel,
                                                                         std::string_view target_binding, glm::uvec3 groups,
                                                                         std::span<const ComputeBinding> bindings,
                                                                         std::span<const std::byte> push_constants,
                                                                         std::span<const ComputeBufferBinding> buffers) {
        ZoneScopedN("Renderer::dispatch_into_offscreen_render_target");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(offscreen_target_error("Cannot write an off-screen render target without an RHI device."));
        }
        const optional<ResolvedOffscreenRenderTarget> resolved = resolve_offscreen_render_target(handle);
        if (!resolved) {
            return unexpected(offscreen_target_error("Cannot write an unknown or destroyed off-screen render target."));
        }
        if (!resolved->storage) {
            return unexpected(offscreen_target_error(
                "This off-screen render target has no storage access (request storage = Required, or use = Compute, at creation)."));
        }

        RenderGraph graph;
        const RenderGraphTextureHandle target = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = resolved->texture,
            .default_view = resolved->view,
            .format = resolved->format,
            .extent = RHI::Extent3D{.width = resolved->extent.x, .height = resolved->extent.y, .depth_or_layers = 1},
            .mip_levels = static_cast<u32>(resolved->mip_views.size()),
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferSrc |
                     RHI::TextureUsage::Storage,
            // Whatever a frame left it in; nothing to keep from a target that was never drawn.
            .initial_layout = resolved->initialized ? RHI::TextureLayout::ShaderReadOnly : RHI::TextureLayout::Undefined,
            .initial_stage = resolved->initialized ? RHI::PipelineStage::AllGraphics | RHI::PipelineStage::ComputeShader
                                                   : RHI::PipelineStage::None,
            .initial_access = resolved->initialized ? RHI::AccessFlags::ShaderRead : RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::ShaderReadOnly,
            .final_stage = RHI::PipelineStage::AllGraphics | RHI::PipelineStage::ComputeShader,
            .final_access = RHI::AccessFlags::ShaderRead,
            .label = "off-screen target (compute write)",
        });
        graph.mark_output(target);

        std::vector<ComputeBinding> all_bindings(bindings.begin(), bindings.end());
        all_bindings.push_back(ComputeBinding{.name = target_binding, .view = resolved->view});
        std::vector<RHI::BindGroupHandle> bind_groups;
        graph.add_compute_pass("off-screen target compute write"_ustr)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = target, .read = false, .write = true})
            .set_execute([this, kernel, &all_bindings, push_constants, groups, &bind_groups,
                          buffers](RenderGraphComputeContext &context) -> Core::RendererResult {
                return record_compute_kernel(context.compute_pass(), kernel, all_bindings, push_constants, groups, bind_groups, buffers);
            });
        if (Core::RendererResult finalize = add_offscreen_target_finalize_passes(graph, target, {}, *resolved, bind_groups);
            !finalize.has_value()) {
            return finalize;
        }

        const auto release = [&](RHI::CommandBufferHandle command_buffer, RHI::FenceHandle fence) {
            graph.destroy_transient_resources(*device);
            for (RHI::BindGroupHandle group : bind_groups) {
                if (group) device->destroy_bind_group(group);
            }
            if (command_buffer) device->destroy_command_buffer(command_buffer);
            if (fence) device->destroy_fence(fence);
        };
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "off-screen target compute write"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create off-screen target compute encoder"));
        }
        if (Core::RendererResult executed = graph.execute(*device, **encoder); !executed.has_value()) {
            release({}, {});
            return executed;
        }
        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            release({}, {});
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish off-screen target compute encoder"));
        }
        auto fence = device->create_fence(RHI::FenceDesc{.label = "off-screen target compute write"});
        if (!fence) {
            release(*command_buffer, {});
            return unexpected(graphics_error_from_rhi(fence.error(), "create off-screen target compute fence"));
        }
        const RHI::CommandBufferHandle command_buffers[1] = {*command_buffer};
        if (auto submitted = device->submit(RHI::SubmitDesc{
                .command_buffers = std::span<const RHI::CommandBufferHandle>{command_buffers, 1},
                .fence = *fence,
                .flags = RHI::SubmitFlags::OneShot,
                .label = "off-screen target compute write",
            });
            !submitted) {
            release(*command_buffer, *fence);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit off-screen target compute write"));
        }
        auto waited = device->wait_fences(std::span<const RHI::FenceHandle>{&*fence, 1}, true);
        release(*command_buffer, *fence);
        if (!waited) {
            return unexpected(graphics_error_from_rhi(waited.error(), "wait for off-screen target compute write"));
        }
        mark_offscreen_render_target_initialized(handle);
        return {};
    }

    /// Returns the current or globally available invalidate offscreen render targets after device loss value.
    ///
    /// @return Returns the current invalidate offscreen render targets after device loss value.
    /// @note This function does not throw exceptions.
    void Renderer::invalidate_offscreen_render_targets_after_device_loss() noexcept {
        ZoneScopedN("Renderer::invalidate_offscreen_render_targets_after_device_loss");
        auto targets = offscreen_render_targets_.lock();
        for (OffscreenRenderTargetRecord &target : *targets) {
            if (!target.alive) {
                continue;
            }
            target.texture = {};
            target.view = {};
            target.mip_views.clear();
            target.sampler = {};
            target.initialized = false;
        }
    }

    /// Returns the current or globally available restore offscreen render targets after recovery value.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::restore_offscreen_render_targets_after_recovery() {
        ZoneScopedN("Renderer::restore_offscreen_render_targets_after_recovery");
        auto targets = offscreen_render_targets_.lock();
        for (OffscreenRenderTargetRecord &target : *targets) {
            if (!target.alive) {
                continue;
            }
            // Resolved again: the new device may offer more (or less) than the old one did.
            auto config = resolve_offscreen_render_target_on_device(target.description);
            if (!config) {
                return unexpected(config.error());
            }
            auto gpu = create_offscreen_render_target_gpu_resources(*config, target.description.label);
            if (!gpu) {
                return unexpected(gpu.error());
            }
            if (!target.sampled_texture || target.sampled_texture.value > textures_.size()) {
                destroy_offscreen_render_target_gpu_resources(*gpu);
                return unexpected(offscreen_target_error(
                    "Cannot restore an off-screen target whose sampling wrapper is missing."));
            }

            TextureResource &sampled = textures_[static_cast<usize>(target.sampled_texture.value - 1)];
            if (!sampled.alive || sampled.handle != target.sampled_texture) {
                destroy_offscreen_render_target_gpu_resources(*gpu);
                return unexpected(offscreen_target_error(
                    "Cannot restore an off-screen target whose sampling wrapper is stale."));
            }

            target.config = std::move(*config);
            target.texture = gpu->texture;
            target.view = gpu->view;
            target.mip_views = gpu->mip_views;
            target.sampler = gpu->sampler;
            target.initialized = false;
            publish_offscreen_render_target(target, sampled);
        }
        return {};
    }

    /// Destroys the all offscreen render targets identified by the supplied parameters.
    ///
    /// @return Returns the current destroy all offscreen render targets value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_all_offscreen_render_targets() noexcept {
        ZoneScopedN("Renderer::destroy_all_offscreen_render_targets");
        auto targets = offscreen_render_targets_.lock();
        for (OffscreenRenderTargetRecord &target : *targets) {
            if (!target.alive) {
                continue;
            }
            if (target.sampled_texture && target.sampled_texture.value <= textures_.size()) {
                TextureResource &sampled = textures_[static_cast<usize>(target.sampled_texture.value - 1)];
                if (sampled.handle == target.sampled_texture) {
                    sampled = {};
                }
            }
            destroy_offscreen_render_target_gpu_objects(target);
            target = {};
        }
        targets->clear();
    }
} // namespace SFT::Renderer
