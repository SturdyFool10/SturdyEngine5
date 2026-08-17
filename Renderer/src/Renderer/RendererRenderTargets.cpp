#include <Foundation/src/Foundation.hpp>

#include <expected>
#include <string>
#include <utility>

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
    } // namespace

    /// Creates a offscreen render target GPU resources from the supplied parameters.
    ///
    /// @param description Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<Renderer::OffscreenRenderTargetGpuResources>
    Renderer::create_offscreen_render_target_gpu_resources(
        const OffscreenRenderTargetDescription &description) {
        ZoneScopedN("Renderer::create_offscreen_render_target_gpu_resources");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(offscreen_target_error(
                "Cannot create an off-screen render target without an RHI device."));
        }
        const char *label = description.label.empty()
            ? "renderer off-screen SDR target"
            : description.label.c_str();

        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::BGRA8UnormSrgb,
            .extent = RHI::Extent3D{
                .width = description.extent.x,
                .height = description.extent.y,
                .depth_or_layers = 1,
            },
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::TransferSrc,
            .label = label,
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create off-screen render target"));
        }

        auto view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .base_mip_level = 0,
            .mip_level_count = 1,
            .label = label,
        });
        if (!view) {
            device->destroy_texture(*texture);
            return unexpected(graphics_error_from_rhi(view.error(), "create off-screen render target view"));
        }

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = label,
        });
        if (!sampler) {
            device->destroy_texture_view(*view);
            device->destroy_texture(*texture);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create off-screen render target sampler"));
        }

        return OffscreenRenderTargetGpuResources{
            .texture = *texture,
            .view = *view,
            .sampler = *sampler,
        };
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
        if (Core::is_zero(description.extent)) {
            return unexpected(offscreen_target_error(
                "Off-screen render targets require a non-zero absolute extent."));
        }
        auto gpu = create_offscreen_render_target_gpu_resources(description);
        if (!gpu) {
            return unexpected(gpu.error());
        }

        const char *label = description.label.empty()
            ? "renderer off-screen SDR target sampling view"
            : description.label.c_str();
        auto sampled_texture = adopt_texture(gpu->texture, gpu->view, gpu->sampler, label);
        if (!sampled_texture) {
            if (RHI::RhiDevice *device = rhi_device()) {
                device->destroy_sampler(gpu->sampler);
                device->destroy_texture_view(gpu->view);
                device->destroy_texture(gpu->texture);
            }
            return unexpected(sampled_texture.error());
        }
        TextureResource *sampled_resource = texture(*sampled_texture);
        if (sampled_resource == nullptr) {
            destroy_texture(*sampled_texture);
            if (RHI::RhiDevice *device = rhi_device()) {
                device->destroy_sampler(gpu->sampler);
                device->destroy_texture_view(gpu->view);
                device->destroy_texture(gpu->texture);
            }
            return unexpected(offscreen_target_error(
                "Failed to retain the off-screen target's sampling wrapper."));
        }
        sampled_resource->externally_destroyable = false;

        auto targets = offscreen_render_targets_.lock();
        const OffscreenRenderTargetHandle handle{static_cast<u64>(targets->size() + 1)};
        targets->push_back(OffscreenRenderTargetRecord{
            .description = description,
            .texture = gpu->texture,
            .view = gpu->view,
            .sampler = gpu->sampler,
            .sampled_texture = *sampled_texture,
            .initialized = false,
            .alive = true,
        });
        return handle;
    }

    /// Destroys the offscreen render target identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
        if (RHI::RhiDevice *device = rhi_device()) {
            if (target.sampler) device->destroy_sampler(target.sampler);
            if (target.view) device->destroy_texture_view(target.view);
            if (target.texture) device->destroy_texture(target.texture);
        }
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
        if (!target.alive || !target.texture || !target.view || Core::is_zero(target.description.extent)) {
            return std::nullopt;
        }
        return ResolvedOffscreenRenderTarget{
            .texture = target.texture,
            .view = target.view,
            .extent = target.description.extent,
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
            auto gpu = create_offscreen_render_target_gpu_resources(target.description);
            if (!gpu) {
                return unexpected(gpu.error());
            }
            if (!target.sampled_texture || target.sampled_texture.value > textures_.size()) {
                if (RHI::RhiDevice *device = rhi_device()) {
                    device->destroy_sampler(gpu->sampler);
                    device->destroy_texture_view(gpu->view);
                    device->destroy_texture(gpu->texture);
                }
                return unexpected(offscreen_target_error(
                    "Cannot restore an off-screen target whose sampling wrapper is missing."));
            }

            TextureResource &sampled = textures_[static_cast<usize>(target.sampled_texture.value - 1)];
            if (!sampled.alive || sampled.handle != target.sampled_texture) {
                if (RHI::RhiDevice *device = rhi_device()) {
                    device->destroy_sampler(gpu->sampler);
                    device->destroy_texture_view(gpu->view);
                    device->destroy_texture(gpu->texture);
                }
                return unexpected(offscreen_target_error(
                    "Cannot restore an off-screen target whose sampling wrapper is stale."));
            }

            target.texture = gpu->texture;
            target.view = gpu->view;
            target.sampler = gpu->sampler;
            target.initialized = false;
            sampled.texture = gpu->texture;
            sampled.view = gpu->view;
            sampled.sampler = gpu->sampler;
            sampled.owns_gpu_resources = false;
            sampled.externally_destroyable = false;
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
        RHI::RhiDevice *device = rhi_device();
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
            if (device != nullptr) {
                if (target.sampler) device->destroy_sampler(target.sampler);
                if (target.view) device->destroy_texture_view(target.view);
                if (target.texture) device->destroy_texture(target.texture);
            }
            target = {};
        }
        targets->clear();
    }
} // namespace SFT::Renderer
