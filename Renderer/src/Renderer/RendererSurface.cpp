#include <Foundation/src/Foundation.hpp>

#include <memory>

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>

using std::unexpected;

namespace SFT::Renderer {

    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::create_window_surface(
        Platform::Windowing::Window &window,
        u32 desired_frames_in_flight) {
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
                // Inherit the app's configured default (recovery_create_info_.features.presentation,
                // stashed at Renderer::initialize() time from the primary window's own RendererCreateInfo)
                // rather than a fresh Core::PresentationSettings{} default — otherwise every runtime-
                // added window (docking tear-off, editor windows) silently ignores whatever vsync/
                // latency/preference the app actually asked for and gets plain VSyncMode::On (Fifo)
                // regardless, while the primary window alone honors it. Real bug: this is exactly what
                // made a torn-off window resolve to plain Fifo on Windows while the primary resolved to
                // FifoRelaxed from the same AdaptiveTearing request — see memory
                // project_adaptive_present_pacing. A caller can still override per-window afterward via
                // set_presentation_settings().
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
        // Move the record out of window_surfaces_ (and off the vector) under the lock, then do the
        // actual GPU teardown unlocked — this window's teardown is real driver work (swapchain/depth/
        // scene-buffer/Hi-Z destruction), and holding the lock across it would block every other
        // window's per-frame window_surface() lookup for as long as this one takes to close.
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
        // WindowSurfaceRecord::scene_frame_resources holds raw RHI::BufferHandle values, not
        // RAII-owning ones — letting `record` go out of scope without this would leak its view/object
        // (and instanced-batch) GPU buffers, same reasoning as destroy_rhi_presentation_resources
        // just above for the swapchain/depth resources.
        destroy_scene_gpu_resources(record->scene_frame_resources);
        // Same reasoning as scene_frame_resources just above: WindowSurfaceRecord::hiz_pyramid also
        // holds raw texture/view handles, not RAII-owning ones.
        destroy_hiz_pyramid(record->hiz_pyramid);
        if (graphics_backend_) {
            graphics_backend_->destroy_window_surface(surface);
        }
    }

    void Renderer::on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        if (graphics_backend_) {
            graphics_backend_->on_surface_resize_needed(surface, extent);
        }
        if (WindowSurfaceRecord *record = window_surface(surface)) {
            record->rhi_swapchain_dirty = true;
        }
    }

    Core::RendererResult Renderer::set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                             const Core::PresentationSettings &settings) {
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }
        record->presentation = settings;
        record->rhi_swapchain_dirty = true;
        return {};
    }

    RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> Renderer::query_hdr_capabilities(
        Core::RenderSurfaceHandle surface) const {
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
        const WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr || !record->rhi_swapchain) {
            // No swapchain yet (first frame not rendered) - default (Fifo/TearFreeOrdered) is the
            // conservative choice for a caller pacing off this: treat an unknown window as vsync-paced
            // rather than assuming it's safe to render uncapped.
            return RHI::PresentationResolution{};
        }
        const RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return RHI::PresentationResolution{};
        }
        return device->presentation_resolution(record->rhi_swapchain);
    }

    FrameTimingSnapshot Renderer::last_frame_timings(Core::RenderSurfaceHandle surface) const noexcept {
        const WindowSurfaceRecord *record = window_surface(surface);
        return record != nullptr ? record->last_frame_timings : FrameTimingSnapshot{};
    }

    Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) noexcept {
        auto guard = window_surfaces_.lock();
        for (auto &record : *guard) {
            if (record->surface == surface) {
                return record.get();
            }
        }
        return nullptr;
    }

    const Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) const noexcept {
        auto guard = window_surfaces_.lock();
        for (auto &record : *guard) {
            if (record->surface == surface) {
                return record.get();
            }
        }
        return nullptr;
    }

} // namespace SFT::Renderer
