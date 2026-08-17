#include <Foundation/src/Foundation.hpp>

#include <memory>

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>

#include <tracy/Tracy.hpp>

using std::unexpected;

namespace SFT::Renderer {

    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::create_window_surface(
        Platform::Windowing::Window &window,
        u32 desired_frames_in_flight) {
        ZoneScopedN("Renderer::create_window_surface");
        if (!graphics_backend_ || !initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Renderer must be initialized before adding a window."});
        }

        auto surface = graphics_backend_->create_window_surface(window, desired_frames_in_flight);
        if (!surface) {
            return unexpected(surface.error());
        }

        {
            auto guard = window_surfaces_.lock();
            guard->push_back(std::make_unique<WindowSurfaceRecord>(WindowSurfaceRecord{
                .window = &window,
                .surface = *surface,
                .desired_frames_in_flight = desired_frames_in_flight,










                .presentation = recovery_create_info_.features.presentation,
                .primary = false,
                .frames_in_flight = {},
            }));
        }
        WindowSurfaceRecord *record = window_surface(*surface);
        if (record == nullptr) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Window surface vanished immediately after registration."});
        }
        if (Core::RendererResult rhi_resources = ensure_rhi_presentation_resources(*record);
            !rhi_resources.has_value()) {
            destroy_window_surface(*surface);
            return unexpected(rhi_resources.error());
        }
        return *surface;
    }

    void Renderer::destroy_window_surface(Core::RenderSurfaceHandle surface) noexcept {
        ZoneScopedN("Renderer::destroy_window_surface");




        unique_ptr<WindowSurfaceRecord> record;
        {
            auto guard = window_surfaces_.lock();
            for (auto it = guard->begin(); it != guard->end(); ++it) {
                if ((*it)->surface == surface) {
                    record = std::move(*it);
                    guard->erase(it);
                    break;
                }
            }
        }
        if (!record) {
            return;
        }
        destroy_rhi_presentation_resources(*record);




        destroy_scene_gpu_resources(record->scene_frame_resources);


        destroy_hiz_pyramid(record->hiz_pyramid);
        if (graphics_backend_) {
            graphics_backend_->destroy_window_surface(surface);
        }
    }

    void Renderer::on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        ZoneScopedN("Renderer::on_surface_resize_needed");
        if (graphics_backend_) {
            graphics_backend_->on_surface_resize_needed(surface, extent);
        }
        if (WindowSurfaceRecord *record = window_surface(surface)) {
            record->rhi_swapchain_dirty = true;
        }
    }

    Core::RendererResult Renderer::set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                             const Core::PresentationSettings &settings) {
        ZoneScopedN("Renderer::set_presentation_settings");
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }
        record->presentation = settings;
        record->rhi_swapchain_dirty = true;
        return {};
    }

    Core::PresentationSettings Renderer::presentation_settings(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::presentation_settings");
        const WindowSurfaceRecord *record = window_surface(surface);
        return record != nullptr ? record->presentation : Core::PresentationSettings{};
    }

    RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> Renderer::query_hdr_capabilities(
        Core::RenderSurfaceHandle surface) const {
        ZoneScopedN("Renderer::query_hdr_capabilities");
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return RHI::rhi_error(RHI::RhiErrorCode::InvalidArgument, "Renderer surface is not registered.");
        }
        const RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "Renderer RHI device is unavailable.");
        }
        if (!record->rhi_surface) {
            return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                  "This window has no RHI surface yet (first frame not rendered).");
        }
        return device->query_hdr_capabilities(record->rhi_surface);
    }

    RHI::RhiResult Renderer::update_hdr_content_light_level(Core::RenderSurfaceHandle surface,
                                                             const RHI::HdrContentLightLevelUpdate &update) {
        ZoneScopedN("Renderer::update_hdr_content_light_level");
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return RHI::rhi_error(RHI::RhiErrorCode::InvalidArgument, "Renderer surface is not registered.");
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "Renderer RHI device is unavailable.");
        }
        if (!record->rhi_swapchain) {
            return RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                  "This window has no live swapchain yet (first frame not rendered).");
        }
        return device->update_hdr_content_light_level(record->rhi_swapchain, update);
    }

    RHI::PresentationResolution Renderer::presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::presentation_resolution");
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr || !record->rhi_swapchain) {



            return RHI::PresentationResolution{};
        }
        const RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return RHI::PresentationResolution{};
        }
        return device->presentation_resolution(record->rhi_swapchain);
    }

    FrameTimingSnapshot Renderer::last_frame_timings(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::last_frame_timings");
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr || !record->last_frame_timings) {
            return {};
        }
        auto snapshot = record->last_frame_timings->lock();
        return *snapshot;
    }

    Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) noexcept {
        ZoneScopedN("Renderer::window_surface");
        auto guard = window_surfaces_.lock();
        for (auto &record : *guard) {
            if (record->surface == surface) {
                return record.get();
            }
        }
        return nullptr;
    }

    const Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::window_surface");
        auto guard = window_surfaces_.lock();
        for (auto &record : *guard) {
            if (record->surface == surface) {
                return record.get();
            }
        }
        return nullptr;
    }

} // namespace SFT::Renderer
