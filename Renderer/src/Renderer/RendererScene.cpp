#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <vector>
#include <glm/vec4.hpp>
#include <Async/src/Async.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/Scene.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::span;
using std::unexpected;

namespace SFT::Renderer {

    namespace {
        [[nodiscard]] Core::GraphicsBackendError scene_error(const char *message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, message};
        }
    } // namespace

    Core::RendererResult Renderer::prepare_scene_gpu_data(
        WindowSurfaceRecord &record, u64 frame_index, const FrameSubmission &submission) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(scene_error("Cannot prepare scene GPU data without an RHI device."));
        }

        // capabilities_.max_frames_in_flight is already >= 1 by construction — resolved exactly once,
        // through Core::resolve_frames_in_flight, at backend initialization (VulkanBackendDevice.cpp).
        const u32 frame_count = capabilities_.max_frames_in_flight;
        if (record.scene_frame_resources.size() != frame_count) {
            destroy_scene_gpu_resources(record.scene_frame_resources);
            record.scene_frame_resources.assign(frame_count, SceneFrameGpuResources{});
        }

        SceneFrameGpuResources &resources = record.scene_frame_resources[frame_index % frame_count];
        if (!resources.view_buffer) {
            auto buffer = device->create_buffer(RHI::BufferDesc{
                .size = sizeof(SceneViewGpuData),
                // Storage (not just Uniform): Shaders/gbuffer_geometry_history.slang binds this as a
                // StructuredBuffer, not a ConstantBuffer<T> — Slang's explicit [[vk::binding(N, 1)]]
                // annotation only reliably respects the requested *set* for StructuredBuffer-kind
                // resources, not ConstantBuffer<T> (observed: the set argument was silently ignored
                // for a ConstantBuffer<T>, while a StructuredBuffer<T> honored it correctly — see that
                // file's own comment). No other consumer binds view_buffer as a real uniform buffer
                // today, so widening its usage costs nothing.
                .usage = RHI::BufferUsage::Uniform | RHI::BufferUsage::Storage,
                .memory = RHI::MemoryLocation::HostUpload,
                .label = "renderer scene view buffer",
            });
            if (!buffer) {
                return unexpected(graphics_error_from_rhi(buffer.error(), "create scene view buffer"));
            }
            resources.view_buffer = *buffer;
        }

        if (!resources.object_buffer || resources.object_capacity < submission.draws.size()) {
            if (resources.object_buffer) {
                device->destroy_buffer(resources.object_buffer);
            }
            resources.object_capacity = std::max<usize>(submission.draws.size(), 1);
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
            .view = submission.camera.view,
            .projection = submission.camera.projection,
            .view_projection = submission.view_projection,
            .previous_view_projection = submission.camera.previous_view_projection,
            .camera_world_position_near = glm::vec4{submission.camera.world_position, submission.camera.near_plane},
            .ambient_radiance_exposure = glm::vec4{submission.lighting.ambient_radiance, submission.lighting.exposure},
            .far_fov_object_count_time = glm::vec4{submission.camera.far_plane,
                                                   submission.camera.vertical_fov_radians,
                                                   static_cast<f32>(submission.draws.size()),
                                                   0.0f},
        };
        if (auto written = device->write_buffer(resources.view_buffer, 0,
                                                std::as_bytes(span<const SceneViewGpuData>{&view_data, 1})); !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "upload scene view buffer"));
        }

        if (submission.draws.empty()) {
            return {};
        }

        std::vector<SceneObjectGpuData> objects(submission.draws.size());
        constexpr usize async_object_packing_threshold = 512;
        const u32 worker_count = Async::Scheduler::worker_count();
        if (submission.draws.size() >= async_object_packing_threshold && worker_count > 1) {
            const usize chunk_count = std::min<usize>(worker_count, submission.draws.size());
            const usize chunk_size = (submission.draws.size() + chunk_count - 1) / chunk_count;
            std::vector<Async::TaskHandle<void>> tasks;
            tasks.reserve(chunk_count);
            for (usize chunk = 0; chunk < chunk_count; ++chunk) {
                const usize begin = chunk * chunk_size;
                const usize end = std::min(submission.draws.size(), begin + chunk_size);
                if (begin >= end) {
                    continue;
                }
                tasks.push_back(Async::Scheduler::spawn([this, &submission, &objects, begin, end]() {
                    for (usize i = begin; i < end; ++i) {
                        const RenderItem &item = submission.draws[i];
                        const auto history = previous_world_transforms_.find(item.stable_id);
                        objects[i] = SceneObjectGpuData{
                            .model = item.world_transform,
                            .previous_model = history != previous_world_transforms_.end()
                                                  ? history->second
                                                  : item.world_transform,
                            .id_sort_visibility_flags = glm::vec4{static_cast<f32>(item.stable_id),
                                                                   static_cast<f32>(item.sort_key),
                                                                   1.0f,
                                                                   0.0f},
                        };
                    }
                }));
            }
            for (const Async::TaskHandle<void> &task : tasks) {
                task.wait();
            }
        } else {
            for (usize i = 0; i < submission.draws.size(); ++i) {
                const RenderItem &item = submission.draws[i];
                const auto history = previous_world_transforms_.find(item.stable_id);
                objects[i] = SceneObjectGpuData{
                    .model = item.world_transform,
                    .previous_model = history != previous_world_transforms_.end() ? history->second
                                                                                   : item.world_transform,
                    .id_sort_visibility_flags = glm::vec4{static_cast<f32>(item.stable_id),
                                                           static_cast<f32>(item.sort_key),
                                                           1.0f,
                                                           0.0f},
                };
            }
        }
        // Overwrite serially, only after every concurrent reader above has joined (the async path's
        // task.wait() loop) — never mutated during the concurrent-read window itself. Becomes *next*
        // frame's history; stable_id (a persistent per-object identity — see its own doc comment on
        // previous_world_transforms_) is the key, not object_index (only this frame's packing
        // position), so an object's history survives being sorted to a different slot.
        for (const RenderItem &item : submission.draws) {
            previous_world_transforms_[item.stable_id] = item.world_transform;
        }
        if (auto written = device->write_buffer(resources.object_buffer, 0,
                                                std::as_bytes(span<const SceneObjectGpuData>{objects.data(), objects.size()})); !written) {
            return unexpected(graphics_error_from_rhi(written.error(), "upload scene object buffer"));
        }
        return {};
    }

    void Renderer::destroy_scene_gpu_resources(vector<SceneFrameGpuResources> &resources) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (SceneFrameGpuResources &frame_resources : resources) {
                if (frame_resources.object_buffer) {
                    device->destroy_buffer(frame_resources.object_buffer);
                }
                if (frame_resources.view_buffer) {
                    device->destroy_buffer(frame_resources.view_buffer);
                }
            }
        }
        resources.clear();
    }

} // namespace SFT::Renderer
