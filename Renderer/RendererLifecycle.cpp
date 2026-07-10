module;

#pragma region Imports
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#pragma endregion

module Sturdy.Renderer;

import :Renderer;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;
import Sturdy.Platform;

using std::array;
using std::optional;
using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] RHI::WindowSystem to_rhi_window_system(Platform::Windowing::NativeWindowSystem system) noexcept {
            switch (system) {
                case Platform::Windowing::NativeWindowSystem::Win32: return RHI::WindowSystem::Win32;
                case Platform::Windowing::NativeWindowSystem::X11: return RHI::WindowSystem::Xlib;
                case Platform::Windowing::NativeWindowSystem::Wayland: return RHI::WindowSystem::Wayland;
                case Platform::Windowing::NativeWindowSystem::Cocoa: return RHI::WindowSystem::Cocoa;
                case Platform::Windowing::NativeWindowSystem::Unknown: break;
            }
            return RHI::WindowSystem::Unknown;
        }

        [[nodiscard]] Core::Extent2D framebuffer_extent(Platform::Windowing::Window &window) {
            if (auto size = window.framebuffer_size()) {
                return Core::Extent2D{size->x, size->y};
            }
            return {};
        }

    } // namespace

    Renderer::Renderer()
        : graphics_backend_(Core::create_vulkan_backend()) {}

    Renderer::~Renderer() {
        wait_idle();
        destroy_all_resources();
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::initialize(
        const Core::RendererCreateInfo &create_info) {
        if (initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Renderer is already initialized."});
        }
        if (!graphics_backend_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::InitializationFailed,
                                                        "No graphics backend is available."});
        }

        auto surface = graphics_backend_->initialize(create_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        initialized_ = true;
        recovery_create_info_ = create_info;
        window_surfaces_.clear();
        window_surfaces_.push_back(WindowSurfaceRecord{
            .window = create_info.window,
            .surface = *surface,
            .desired_frames_in_flight = create_info.features.desired_frames_in_flight,
            .primary = true,
        });
        capabilities_ = graphics_backend_->capabilities();

        if (create_info.window != nullptr) {
            if (Core::RendererResult rhi_resources = ensure_rhi_presentation_resources(window_surfaces_.back());
                !rhi_resources.has_value()) {
                destroy_window_surface(*surface);
                initialized_ = false;
                return unexpected(rhi_resources.error());
            }
        }
        return *surface;
    }

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

        window_surfaces_.push_back(WindowSurfaceRecord{
            .window = &window,
            .surface = *surface,
            .desired_frames_in_flight = desired_frames_in_flight,
            .primary = false,
        });
        if (Core::RendererResult rhi_resources = ensure_rhi_presentation_resources(window_surfaces_.back());
            !rhi_resources.has_value()) {
            destroy_window_surface(*surface);
            return unexpected(rhi_resources.error());
        }
        return *surface;
    }

    void Renderer::destroy_window_surface(Core::RenderSurfaceHandle surface) noexcept {
        if (graphics_backend_) {
            graphics_backend_->destroy_window_surface(surface);
        }

        for (auto it = window_surfaces_.begin(); it != window_surfaces_.end(); ++it) {
            if (it->surface == surface) {
                destroy_rhi_presentation_resources(*it);
                window_surfaces_.erase(it);
                break;
            }
        }
    }

    void Renderer::on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept {
        if (graphics_backend_) {
            graphics_backend_->on_surface_resize_needed(surface);
        }
        if (WindowSurfaceRecord *record = window_surface(surface)) {
            record->rhi_swapchain_dirty = true;
        }
    }

    Core::RendererResult Renderer::render_frame(Core::RenderSurfaceHandle surface,
                                                const Core::FrameInput &frame) {
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }

        auto submit_frame = [&]() -> Core::RendererResult {
            return render_frame_rhi(*record, frame);
        };

        Core::RendererResult result = submit_frame();
        if (result.has_value() || result.error().code != Core::GraphicsBackendErrorCode::DeviceLost) {
            return result;
        }

        Core::RendererResult recovery = recover_from_device_loss();
        if (!recovery.has_value()) {
            return recovery;
        }

        record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is unavailable after device-loss recovery.");
        }
        return render_frame_rhi(*record, frame);
    }

    Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) noexcept {
        for (WindowSurfaceRecord &record : window_surfaces_) {
            if (record.surface == surface) {
                return &record;
            }
        }
        return nullptr;
    }

    const Renderer::WindowSurfaceRecord *Renderer::window_surface(Core::RenderSurfaceHandle surface) const noexcept {
        for (const WindowSurfaceRecord &record : window_surfaces_) {
            if (record.surface == surface) {
                return &record;
            }
        }
        return nullptr;
    }

    Core::RendererResult Renderer::ensure_rhi_presentation_resources(WindowSurfaceRecord &record) {
        if (record.rhi_surface && record.rhi_swapchain) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (record.window == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI presentation requires a live window.");
        }

        if (!record.rhi_surface) {
            const auto native = record.window->native_window_handle();
            if (!native) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::InitializationFailed,
                                                    string("Failed to query native window handle for RHI surface: ") + native.error().message);
            }

            RHI::SurfaceDesc surface_desc{
                .system = to_rhi_window_system(native->system),
                .display = native->display,
                .window = native->window,
                .label = "renderer window surface",
            };
            auto surface = device->create_surface(surface_desc);
            if (!surface) {
                return unexpected(graphics_error_from_rhi(surface.error(), "create RHI surface"));
            }
            record.rhi_surface = *surface;
        }

        return recreate_rhi_swapchain(record);
    }

    Core::RendererResult Renderer::recreate_rhi_swapchain(WindowSurfaceRecord &record) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (!record.rhi_surface) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an RHI swapchain without an RHI surface.");
        }
        if (record.window == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an RHI swapchain without a live window.");
        }

        const Core::Extent2D extent = framebuffer_extent(*record.window);
        if (extent.is_zero()) {
            record.rhi_swapchain_dirty = true;
            return {};
        }

        const RHI::SwapchainHandle old_swapchain = record.rhi_swapchain;

        RHI::SwapchainDesc swapchain_desc{
            .surface = record.rhi_surface,
            .width = extent.width,
            .height = extent.height,
            .format = RHI::Format::BGRA8UnormSrgb,
            .present_mode = RHI::PresentMode::Fifo,
            .usage = RHI::TextureUsage::ColorAttachment,
            .composite_alpha = RHI::CompositeAlphaMode::Auto,
            .clipped = true,
            .image_count = record.desired_frames_in_flight,
            .old_swapchain = old_swapchain,
            .label = "renderer swapchain",
        };
        auto swapchain = device->create_swapchain(swapchain_desc);
        if (!swapchain) {
            return unexpected(graphics_error_from_rhi(swapchain.error(), "create RHI swapchain"));
        }

        record.rhi_swapchain = *swapchain;
        record.swapchain_extent = extent;
        record.rhi_swapchain_dirty = false;
        if (old_swapchain) {
            device->destroy_swapchain(old_swapchain);
        }
        return {};
    }

    Core::RendererResult Renderer::render_frame_rhi(WindowSurfaceRecord &record,
                                                    const Core::FrameInput &frame) {
        (void)frame;

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }

        if (Core::RendererResult resources = ensure_rhi_presentation_resources(record); !resources.has_value()) {
            return resources;
        }

        const Core::Extent2D extent = record.window != nullptr ? framebuffer_extent(*record.window) : Core::Extent2D{};
        if (extent.is_zero()) {
            return {};
        }
        if (record.rhi_swapchain_dirty || extent.width != record.swapchain_extent.width ||
            extent.height != record.swapchain_extent.height) {
            if (Core::RendererResult recreated = recreate_rhi_swapchain(record); !recreated.has_value()) {
                return recreated;
            }
        }
        if (!record.rhi_swapchain) {
            return {};
        }

        auto acquired = device->acquire_next_texture(record.rhi_swapchain);
        if (!acquired) {
            if (acquired.error().code == RHI::RhiErrorCode::SurfaceLost) {
                record.rhi_swapchain_dirty = true;
            }
            return unexpected(graphics_error_from_rhi(acquired.error(), "acquire RHI swapchain texture"));
        }
        RHI::SurfaceTexture texture = *acquired;
        if (texture.suboptimal) {
            record.rhi_swapchain_dirty = true;
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer frame"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create RHI command encoder"));
        }

        const RHI::TextureBarrier to_color{
            .texture = texture.texture,
            .src_stage = RHI::PipelineStage::None,
            .src_access = RHI::AccessFlags::None,
            .dst_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .dst_access = RHI::AccessFlags::ColorAttachmentWrite,
            .old_layout = RHI::TextureLayout::Undefined,
            .new_layout = RHI::TextureLayout::ColorAttachment,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_color, 1});

        const RHI::ColorAttachment color_attachment{
            .view = texture.view,
            .load_op = RHI::LoadOp::Clear,
            .store_op = RHI::StoreOp::Store,
            .clear_color = RHI::ClearColor{0.01f, 0.015f, 0.025f, 1.0f},
        };
        const RHI::RenderPassDesc pass_desc{
            .color_attachments = span<const RHI::ColorAttachment>{&color_attachment, 1},
            .render_area = RHI::Rect2D{.x = 0, .y = 0, .width = extent.width, .height = extent.height},
            .label = "renderer main pass",
        };
        auto pass = (*encoder)->begin_render_pass(pass_desc);
        if (!pass) {
            return unexpected(graphics_error_from_rhi(pass.error(), "begin RHI render pass"));
        }
        (*pass)->end();

        const RHI::TextureBarrier to_present{
            .texture = texture.texture,
            .src_stage = RHI::PipelineStage::ColorAttachmentOutput,
            .src_access = RHI::AccessFlags::ColorAttachmentWrite,
            .dst_stage = RHI::PipelineStage::None,
            .dst_access = RHI::AccessFlags::None,
            .old_layout = RHI::TextureLayout::ColorAttachment,
            .new_layout = RHI::TextureLayout::Present,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_present, 1});

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish RHI command encoder"));
        }

        auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer frame fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create RHI frame fence"));
        }

        const array command_buffers{*command_buffer};
        const array presented_textures{texture};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .waits = {},
            .signals = {},
            .presented_textures = span<const RHI::SurfaceTexture>{presented_textures.data(), presented_textures.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer frame submit",
        };
        if (auto submitted = device->submit(submit_desc); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit RHI frame"));
        }

        if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true); !waited) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(waited.error(), "wait RHI frame fence"));
        }
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);

        auto presented = device->present(RHI::PresentDesc{.texture = texture, .label = "renderer present"});
        if (!presented) {
            if (presented.error().code == RHI::RhiErrorCode::SurfaceLost) {
                record.rhi_swapchain_dirty = true;
            }
            return unexpected(graphics_error_from_rhi(presented.error(), "present RHI frame"));
        }
        if (*presented) {
            record.rhi_swapchain_dirty = true;
        }
        return {};
    }

    void Renderer::destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            if (record.rhi_swapchain) {
                device->destroy_swapchain(record.rhi_swapchain);
            }
            if (record.rhi_surface) {
                device->destroy_surface(record.rhi_surface);
            }
        }
        record.rhi_swapchain = {};
        record.rhi_surface = {};
        record.swapchain_extent = {};
        record.rhi_swapchain_dirty = true;
    }

    void Renderer::wait_idle() noexcept {
        if (graphics_backend_) {
            graphics_backend_->wait_idle();
        }
    }

    const RHI::FeatureNegotiationReport *Renderer::feature_negotiation_report() const noexcept {
        const RHI::RhiDevice *device = rhi_device();
        return device != nullptr ? &device->feature_negotiation_report() : nullptr;
    }

    optional<Core::GpuInfo> Renderer::gpu_info() const {
        if (!graphics_backend_) {
            return std::nullopt;
        }
        return graphics_backend_->gpu_info();
    }

    RHI::RhiDevice *Renderer::rhi_device() noexcept {
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

    const RHI::RhiDevice *Renderer::rhi_device() const noexcept {
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

} // namespace SFT::Renderer
