#include <Foundation/Foundation.hpp>

#include <memory>

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/WindowManager.hpp>

#include <tracy/Tracy.hpp>

using std::unexpected;

namespace SFT::Renderer {

    /// Creates a window surface from the supplied parameters.
    ///
    /// @param window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::create_window_surface(
        WindowManager::Window &window,
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

    /// Destroys the window surface identified by the supplied parameters.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Handles the on surface resize needed callback and updates the associated platform state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        ZoneScopedN("Renderer::on_surface_resize_needed");
        if (graphics_backend_) {
            graphics_backend_->on_surface_resize_needed(surface, extent);
        }
        if (WindowSurfaceRecord *record = window_surface(surface)) {
            record->rhi_swapchain_dirty = true;
        }
    }

    /// Sets the presentation settings for this `Renderer`.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Returns `surface`'s current presentation policy (vsync/HDR/transparent-composition/...).
    ///
    /// @param surface Surface to query.
    ///
    /// @return The last value set_presentation_settings() accepted for this surface, or the app-wide
    ///         default it was created with if never overridden; default-constructed
    ///         Core::PresentationSettings{} when `surface` isn't registered.
    /// @note This function does not throw exceptions.
    Core::PresentationSettings Renderer::presentation_settings(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::presentation_settings");
        const WindowSurfaceRecord *record = window_surface(surface);
        return record != nullptr ? record->presentation : Core::PresentationSettings{};
    }

    /// Queries HDR capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::OperationFailed`.
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

    /// Updates HDR content light level from the supplied values.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param update `update` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::OperationFailed`.
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

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Performs the last frame timings operation for `Renderer` using the supplied arguments.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    FrameTimingSnapshot Renderer::last_frame_timings(Core::RenderSurfaceHandle surface) const noexcept {
        ZoneScopedN("Renderer::last_frame_timings");
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr || !record->last_frame_timings) {
            return {};
        }
        auto snapshot = record->last_frame_timings->lock();
        return *snapshot;
    }

    /// Performs the window surface operation for `Renderer` using the supplied arguments.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
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

    /// Performs the window surface operation for `Renderer` using the supplied arguments.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
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
