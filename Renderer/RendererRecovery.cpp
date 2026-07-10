module;

#pragma region Imports
#include <expected>
#include <string>
#pragma endregion

module Sturdy.Renderer;

import :Renderer;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;
import Sturdy.Platform;

using std::string;
using std::unexpected;

namespace SFT::Renderer {

    Core::RendererResult Renderer::recover_from_device_loss() {
        if (recovering_from_device_loss_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::DeviceLost,
                                                "Renderer is already recovering from GPU device loss.");
        }

        if (!initialized_ || recovery_create_info_.window == nullptr || window_surfaces_.empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::DeviceLost,
                                                "Cannot recover from GPU device loss before renderer initialization state is available.");
        }

        recovering_from_device_loss_ = true;

        // Device-lost teardown cannot safely call RHI destroy_* for renderer-owned resources: the old
        // logical device is invalid. Drop the high-level GPU handles first, then release the backend and
        // rebuild a fresh one from the retained creation/window state.
        invalidate_gpu_resource_handles_no_destroy();
        graphics_backend_.reset();
        graphics_backend_ = Core::create_vulkan_backend();
        if (!graphics_backend_) {
            recovering_from_device_loss_ = false;
            initialized_ = false;
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::InitializationFailed,
                                                "Failed to create a replacement graphics backend after GPU device loss.");
        }

        Core::RendererExpected<Core::RenderSurfaceHandle> primary = graphics_backend_->initialize(recovery_create_info_);
        if (!primary) {
            recovering_from_device_loss_ = false;
            initialized_ = false;
            return unexpected(Core::GraphicsBackendError{
                .code = primary.error().code,
                .message = string("Failed to reinitialize graphics backend after GPU device loss: ") + primary.error().message,
            });
        }

        for (WindowSurfaceRecord &record : window_surfaces_) {
            if (record.primary) {
                record.surface = *primary;
                continue;
            }
            if (record.window == nullptr) {
                continue;
            }

            Core::RendererExpected<Core::RenderSurfaceHandle> recreated =
                graphics_backend_->create_window_surface(*record.window, record.desired_frames_in_flight);
            if (!recreated) {
                recovering_from_device_loss_ = false;
                initialized_ = false;
                return unexpected(Core::GraphicsBackendError{
                    .code = recreated.error().code,
                    .message = string("Failed to recreate renderer window surface after GPU device loss: ") + recreated.error().message,
                });
            }
            record.surface = *recreated;
        }

        capabilities_ = graphics_backend_->capabilities();
        Core::RendererResult restored = restore_gpu_resources_after_recovery();
        recovering_from_device_loss_ = false;
        if (!restored.has_value()) {
            initialized_ = false;
            return restored;
        }

        initialized_ = true;
        return {};
    }

    void Renderer::invalidate_gpu_resource_handles_no_destroy() noexcept {
        for (MeshResource &mesh : meshes_) {
            mesh.vertex_buffer = {};
            mesh.index_buffer = {};
            mesh.gpu_resident = false;
        }

        for (TextureResource &texture : textures_) {
            texture.texture = {};
            texture.view = {};
            texture.sampler = {};
        }
    }

    Core::RendererResult Renderer::restore_gpu_resources_after_recovery() {
        for (MeshResource &mesh : meshes_) {
            if (!mesh.alive) {
                continue;
            }
            Core::RendererResult uploaded = try_upload_mesh(mesh);
            if (!uploaded.has_value()) {
                return uploaded;
            }
        }

        // Texture/material GPU restoration will be added when texture/material creation owns enough
        // source data to recreate their RHI objects. Materials are CPU-side records today.
        return {};
    }

} // namespace SFT::Renderer
