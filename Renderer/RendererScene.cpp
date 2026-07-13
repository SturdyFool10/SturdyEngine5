module;

#pragma region Imports
#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <vector>
#include <glm/vec4.hpp>
#pragma endregion

module Sturdy.Renderer;

import :Renderer;
import :Scene;
import Sturdy.Core;
import Sturdy.RHI;

using std::span;
using std::unexpected;

namespace SFT::Renderer {

    namespace {
        [[nodiscard]] Core::GraphicsBackendError scene_error(const char *message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, message};
        }
    } // namespace

    Core::RendererResult Renderer::prepare_scene_gpu_data(u64 frame_index) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(scene_error("Cannot prepare scene GPU data without an RHI device."));
        }

        const u32 frame_count = capabilities_.max_frames_in_flight == 0 ? 1u : capabilities_.max_frames_in_flight;
        if (scene_frame_resources_.size() != frame_count) {
            destroy_scene_gpu_resources();
            scene_frame_resources_.assign(frame_count, SceneFrameGpuResources{});
        }

        SceneFrameGpuResources &resources = scene_frame_resources_[frame_index % frame_count];
        if (!resources.view_buffer) {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = sizeof(SceneViewGpuData),
                .usage = RHI::BufferUsage::Uniform,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "renderer scene view buffer",
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), "create scene view buffer"));
            }
            resources.view_buffer = *buffer;
        }

        if (!resources.object_buffer || resources.object_capacity < frame_draws_.size()) {
            if (resources.object_buffer) {
                device->destroy_buffer(resources.object_buffer);
            }
            resources.object_capacity = std::max<usize>(frame_draws_.size(), 1);
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = static_cast<u64>(resources.object_capacity * sizeof(SceneObjectGpuData)),
                .usage = RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "renderer scene object buffer",
            });
            if (!buffer) {
                resources.object_buffer = {};
                resources.object_capacity = 0;
                return unexpected(graphics_error_from_rhi(buffer.error(), "create scene object buffer"));
            }
            resources.object_buffer = *buffer;
        }

        const SceneViewGpuData view_data{
            .view = frame_camera_.view,
            .projection = frame_camera_.projection,
            .view_projection = frame_view_projection_,
            .camera_world_position_near = glm::vec4{frame_camera_.world_position, frame_camera_.near_plane},
            .ambient_radiance_exposure = glm::vec4{frame_lighting_.ambient_radiance, frame_lighting_.exposure},
            .far_fov_object_count_time = glm::vec4{frame_camera_.far_plane,
                                                   frame_camera_.vertical_fov_radians,
                                                   static_cast<f32>(frame_draws_.size()),
                                                   0.0f},
        };
        if (auto written = device->write_buffer(resources.view_buffer, 0,
                                                std::as_bytes(span<const SceneViewGpuData>{&view_data, 1})); !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "upload scene view buffer"));
        }

        if (frame_draws_.empty()) {
            return {};
        }

        std::vector<SceneObjectGpuData> objects;
        objects.reserve(frame_draws_.size());
        for (const RenderItem &item : frame_draws_) {
            objects.push_back(SceneObjectGpuData{
                .model = item.world_transform,
                .previous_model = item.world_transform,
                .id_sort_visibility_flags = glm::vec4{static_cast<f32>(item.stable_id),
                                                       static_cast<f32>(item.sort_key),
                                                       1.0f,
                                                       0.0f},
            });
        }
        if (auto written = device->write_buffer(resources.object_buffer, 0,
                                                std::as_bytes(span<const SceneObjectGpuData>{objects.data(), objects.size()})); !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "upload scene object buffer"));
        }
        return {};
    }

    void Renderer::destroy_scene_gpu_resources() noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (SceneFrameGpuResources &resources : scene_frame_resources_) {
                if (resources.object_buffer) {
                    device->destroy_buffer(resources.object_buffer);
                }
                if (resources.view_buffer) {
                    device->destroy_buffer(resources.view_buffer);
                }
            }
        }
        scene_frame_resources_.clear();
    }

} // namespace SFT::Renderer
