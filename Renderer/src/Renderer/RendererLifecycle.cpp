#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/Scene.hpp>
#include <Renderer/RenderGraph.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>

using std::array;
using std::chrono::duration;
using std::chrono::steady_clock;
using std::optional;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {

        constexpr f64 renderer_stage_hitch_threshold_seconds = 0.050;
        // How many superseded swapchains a window's frame-in-flight ring tolerates before
        // maybe_flush_retired_swapchains() pays one bounded wait_idle() to clear them out — see that
        // function's declaration comment. Small enough to bound worst-case leaked-handle count during
        // a long continuous resize drag, large enough that it doesn't trigger on ordinary one-off
        // resizes (which retire exactly one swapchain and get reclaimed the normal way soon after).
        //
        // Investigated as a possible cause of multi-second vkCreateSwapchainKHR stalls seen during
        // Windows interactive-resize testing (see Application::render_managed_window's
        // wait_for_completion doc) — direct instrumentation ruled it out: forcing this down to 1
        // (flush after every single resize) did not change the stall's timing or frequency at all,
        // and the flush's own wait_idle() consistently measured well under a millisecond. The stall
        // is isolated entirely inside the driver's vkCreateSwapchainKHR call itself (confirmed by
        // wrapping just that call), correlates with a preceding multi-second gap of zero swapchain
        // activity, and fully recovers on the very next call — the signature of a GPU power-state
        // wake-up cost, not an application-side resource backlog. Left at the original tolerance;
        // see render_managed_window's adaptive synchronous-repaint fallback for the actual mitigation.
        constexpr usize retired_swapchain_flush_threshold = 6;

        class ScopedRendererStageTimer {
          public:
            explicit ScopedRendererStageTimer(const char *stage) noexcept
                : stage_(stage), start_(steady_clock::now()) {}

            // `accumulate_into`: when non-null, this stage's duration is also appended (in
            // milliseconds) so a caller can build a full per-frame CPU stage breakdown — the
            // hitch-warning behavior above is unconditional either way, this is purely additive.
            ScopedRendererStageTimer(const char *stage, vector<std::pair<string, f64>> *accumulate_into) noexcept
                : stage_(stage), start_(steady_clock::now()), accumulate_into_(accumulate_into) {}

            ~ScopedRendererStageTimer() noexcept {
                const f64 seconds = duration<f64>(steady_clock::now() - start_).count();
                if (seconds >= renderer_stage_hitch_threshold_seconds) {
                    Foundation::log_warn("Renderer stage '{}' took {}", stage_, Foundation::human_readable_time(seconds));
                }
                if (accumulate_into_ != nullptr) {
                    accumulate_into_->emplace_back(string{stage_}, seconds * 1000.0);
                }
            }

          private:
            const char *stage_;
            steady_clock::time_point start_;
            vector<std::pair<string, f64>> *accumulate_into_ = nullptr;
        };

        // Collapses a pass label's numbered-instance suffix (for example, a bloom mip level) down
        // to its category by truncating at the first digit, so the GPU/CPU timing breakdowns sum
        // same-kind passes into one line. Labels with no digit pass through unchanged.
        [[nodiscard]] string render_graph_pass_timing_category(std::string_view label) noexcept {
            const usize digit = label.find_first_of("0123456789");
            string category{digit == std::string_view::npos ? label : label.substr(0, digit)};
            while (!category.empty() && category.back() == ' ') {
                category.pop_back();
            }
            return category;
        }

        [[nodiscard]] Core::Extent2D framebuffer_extent(Platform::Windowing::Window &window) {
            if (auto size = window.framebuffer_size()) {
                return Core::Extent2D{size->x, size->y};
            }
            return {};
        }

        [[maybe_unused]] [[nodiscard]] Core::GraphicsBackendError graphics_error_from_shader(const Core::Slang::ShaderError &error,
                                                                                            const char *operation) {
            string message = string(operation) + " failed: " + error.message;
            if (!error.diagnostics.empty()) {
                message += "\n";
                message += error.diagnostics;
            }
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

    } // namespace

    Renderer::Renderer() = default;

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
            graphics_backend_ = Core::create_vulkan_backend();
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
        {
            auto guard = window_surfaces_.lock();
            guard->clear();
            guard->push_back(std::make_unique<WindowSurfaceRecord>(WindowSurfaceRecord{
                .window = create_info.window,
                .surface = *surface,
                .desired_frames_in_flight = create_info.features.desired_frames_in_flight,
                .presentation = create_info.features.presentation,
                .primary = true,
                .frames_in_flight = {},
            }));
        }
        capabilities_ = graphics_backend_->capabilities();

        if (create_info.window != nullptr) {
            WindowSurfaceRecord *record = window_surface(*surface);
            Core::RendererResult rhi_resources = record != nullptr
                ? ensure_rhi_presentation_resources(*record)
                : Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                               "Primary window surface vanished immediately after registration.");
            if (!rhi_resources.has_value()) {
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

        {
            auto guard = window_surfaces_.lock();
            guard->push_back(std::make_unique<WindowSurfaceRecord>(WindowSurfaceRecord{
                .window = &window,
                .surface = *surface,
                .desired_frames_in_flight = desired_frames_in_flight,
                .presentation = Core::PresentationSettings{},
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
        auto guard = window_surfaces_.lock();
        for (auto it = guard->begin(); it != guard->end(); ++it) {
            if ((*it)->surface == surface) {
                destroy_rhi_presentation_resources(**it);
                if (graphics_backend_) {
                    graphics_backend_->destroy_window_surface(surface);
                }
                guard->erase(it);
                break;
            }
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

    Core::RendererResult Renderer::submit_draw(MeshHandle mesh_handle, MaterialInstanceHandle material_handle) {
        if (mesh(mesh_handle) == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot submit a draw for an unknown mesh.");
        }
        if (material_instance(material_handle) == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot submit a draw for an unknown material instance.");
        }
        frame_draws_.push_back(RenderItem{.mesh = mesh_handle, .material = material_handle});
        return {};
    }

    Core::RendererResult Renderer::render_frame(const RenderFrameDesc &desc) {
        // Dev-time shader hot-reload: pick up any edited `.slang` file and rebuild the affected material
        // templates before recording. Cheap when nothing changed (a directory stat); the reload path
        // itself does the one sanctioned wait_idle (see plans/shader-variants-and-hot-reload.md).
        poll_shader_hot_reload();

        FrameSubmission submission{};
        submission.camera = desc.view.camera;
        submission.lighting = desc.view.lighting;
        submission.deferred_formats = desc.view.deferred_formats;
        submission.render_graph = desc.view.render_graph;
        submission.view_projection = desc.view.camera.projection * desc.view.camera.view;
        submission.debug_label = desc.view.debug_label;

        {
            ScopedRendererStageTimer timer{"extract render items",
                                           desc.view.render_graph.debug_overlay ? &submission.pre_dispatch_stage_timings_ms : nullptr};
            submission.draws.reserve(desc.view.renderables.size());
            for (const SceneRenderable &renderable : desc.view.renderables) {
                if ((renderable.visibility_mask & desc.view.visibility_mask) == 0) {
                    continue;
                }
                if (mesh(renderable.mesh) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Scene renderable references an unknown mesh.");
                }
                if (material_instance(renderable.material) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Scene renderable references an unknown material instance.");
                }
                submission.draws.push_back(RenderItem{
                    .mesh = renderable.mesh,
                    .material = renderable.material,
                    .world_transform = renderable.world_transform,
                    .stable_id = renderable.stable_id,
                    .sort_key = renderable.sort_key,
                });
            }

            // Gizmos are never visibility-mask-filtered (a dev aid, not gameplay-visibility-relevant).
            submission.gizmo_draws.reserve(desc.view.gizmo_renderables.size());
            for (const SceneRenderable &renderable : desc.view.gizmo_renderables) {
                if (mesh(renderable.mesh) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Gizmo renderable references an unknown mesh.");
                }
                if (material_instance(renderable.material) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Gizmo renderable references an unknown material instance.");
                }
                submission.gizmo_draws.push_back(RenderItem{
                    .mesh = renderable.mesh,
                    .material = renderable.material,
                    .world_transform = renderable.world_transform,
                    .stable_id = renderable.stable_id,
                    .sort_key = renderable.sort_key,
                });
            }
        }

        return render_frame_dispatch(desc.surface, desc.frame, submission);
    }

    Core::RendererResult Renderer::render_frame(Core::RenderSurfaceHandle surface,
                                                const Core::FrameInput &frame) {
        // Dev-time shader hot-reload: pick up any edited `.slang` file and rebuild the affected material
        // templates before recording. Cheap when nothing changed (a directory stat); the reload path
        // itself does the one sanctioned wait_idle (see plans/shader-variants-and-hot-reload.md).
        poll_shader_hot_reload();

        // Legacy path for the public submit_draw() API — see frame_draws_'s doc comment on why this is
        // the only caller left that still touches it.
        FrameSubmission submission{};
        submission.draws = std::move(frame_draws_);
        frame_draws_.clear();

        return render_frame_dispatch(surface, frame, submission);
    }

    Core::RendererResult Renderer::render_frame_dispatch(Core::RenderSurfaceHandle surface,
                                                          const Core::FrameInput &frame,
                                                          FrameSubmission &submission) {
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }

        // Group draws by (material, mesh) so every geometry pass below sees runs of consecutive
        // items that share a pipeline/bind-group/vertex-buffer,
        // which record_render_item's binding_state then skips rebinding for (see its doc comment).
        {
            ScopedRendererStageTimer timer{"sort render items",
                                           submission.render_graph.debug_overlay ? &submission.pre_dispatch_stage_timings_ms : nullptr};
            std::sort(submission.draws.begin(), submission.draws.end(), [](const RenderItem &a, const RenderItem &b) {
                if (!(a.material == b.material)) {
                    return a.material.value < b.material.value;
                }
                return a.mesh.value < b.mesh.value;
            });
        }

        Core::RendererResult result = render_frame_rhi(*record, frame, submission);
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
        return render_frame_rhi(*record, frame, submission);
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
            if (!graphics_backend_) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Renderer graphics backend is unavailable.");
            }
            auto surface = graphics_backend_->rhi_surface_for(record.surface);
            if (!surface) {
                return unexpected(surface.error());
            }
            record.rhi_surface = *surface;
        }

        return recreate_rhi_swapchain(record);
    }

    bool Renderer::render_item_visible(const RenderItem &item, const Frustum &frustum) noexcept {
        const MeshResource *mesh_resource = mesh(item.mesh);
        if (mesh_resource == nullptr) {
            return true;
        }
        const f32 scale_x = glm::length(glm::vec3{item.world_transform[0]});
        const f32 scale_y = glm::length(glm::vec3{item.world_transform[1]});
        const f32 scale_z = glm::length(glm::vec3{item.world_transform[2]});
        const f32 max_scale = std::max({scale_x, scale_y, scale_z});
        const glm::vec3 world_center =
            glm::vec3{item.world_transform * glm::vec4{mesh_resource->bounds_center, 1.0f}};
        return frustum_intersects_sphere(frustum, world_center, mesh_resource->bounds_radius * max_scale);
    }

    template <typename Encoder>
    Core::RendererResult Renderer::record_render_item(Encoder &pass,
                                                      const RenderItem &item,
                                                      span<const RHI::Format> color_formats,
                                                      RHI::Format depth_format,
                                                      u64 frame_index,
                                                      const glm::mat4 &view_projection,
                                                      bool depth_only,
                                                      RenderItemBindingState &binding_state,
                                                      bool standard_depth_test,
                                                      bool shadow_map,
                                                      f32 shadow_depth_bias,
                                                      f32 shadow_slope_bias) {
        MeshResource *mesh_resource = mesh(item.mesh);
        if (mesh_resource == nullptr || !mesh_resource->gpu_resident || !vertex_arena_.buffer) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Render item references a mesh that is not GPU-resident.");
        }

        MaterialInstanceResource *material_resource = material_instance(item.material);
        if (material_resource == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Render item references an unknown material instance.");
        }
        MaterialTemplateResource *material_template_resource = material_template(material_resource->material_template);
        if (material_template_resource == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Render item material references an unknown material template.");
        }

        auto pipeline = depth_only
                            ? depth_only_pipeline_for(*material_template_resource, depth_format, shadow_map,
                                                      shadow_depth_bias, shadow_slope_bias)
                            : material_pipeline_for(*material_template_resource, color_formats, depth_format, standard_depth_test);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        // Redundant-state elision: submission.draws is sorted by (material, mesh) before any pass
        // records it (see render_frame_dispatch), so consecutive RenderItems very often share a
        // pipeline/bind-group/vertex-buffer — skip reissuing state that's already bound in this pass.
        if (!(binding_state.pipeline == *pipeline)) {
            pass.set_pipeline(*pipeline);
            binding_state.pipeline = *pipeline;
        }

        const SceneDrawConstants draw_constants{
            .view_projection = view_projection,
            .model = item.world_transform,
        };
        pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                std::as_bytes(span<const SceneDrawConstants>{&draw_constants, 1}));

        const u32 frame_slot = material_resource->frames.empty()
                                    ? 0u
                                    : static_cast<u32>(frame_index % material_resource->frames.size());
        if (!material_resource->frames.empty() &&
            (!(binding_state.material == item.material) || binding_state.material_frame_slot != frame_slot)) {
            auto bind_groups = prepare_material_frame(*material_resource, frame_slot);
            if (!bind_groups) {
                return unexpected(bind_groups.error());
            }
            for (usize i = 0; i < bind_groups->size() && i < material_template_resource->bind_group_layout_sets.size(); ++i) {
                pass.set_bind_group(material_template_resource->bind_group_layout_sets[i], (*bind_groups)[i]);
            }
            binding_state.material = item.material;
            binding_state.material_frame_slot = frame_slot;
        }

        // Every mesh lives in the same shared vertex/index arena (see try_upload_mesh), so the buffer
        // binding itself is constant for the whole pass regardless of which mesh is being drawn —
        // only the per-draw base_vertex/first_index offset changes. binding_state.mesh here is really
        // "has *any* draw in this pass bound the arena yet", not a per-mesh rebind.
        if (!binding_state.arena_bound) {
            pass.set_vertex_buffer(0, vertex_arena_.buffer);
            if (index_arena_.buffer) {
                pass.set_index_buffer(index_arena_.buffer, RHI::IndexFormat::Uint32);
            }
            binding_state.arena_bound = true;
        }
        if (index_arena_.buffer && !mesh_resource->indices.empty()) {
            pass.draw_indexed(RHI::DrawIndexedArgs{
                .index_count = static_cast<u32>(mesh_resource->indices.size()),
                .first_index = mesh_resource->index_offset,
                .base_vertex = static_cast<i32>(mesh_resource->vertex_offset),
            });
        } else {
            pass.draw(RHI::DrawArgs{
                .vertex_count = static_cast<u32>(mesh_resource->vertices.size()),
                .first_vertex = mesh_resource->vertex_offset,
            });
        }
        return {};
    }

    namespace {
        // Below this many surviving (post-frustum-cull) items, recording directly against the
        // primary pass wins outright — spinning up per-thread RenderBundleEncoders costs more than
        // it saves. Chosen the same order of magnitude as prepare_scene_gpu_data's own
        // async-packing threshold (RendererScene.cpp), which faces the same per-item-vs-per-task
        // overhead tradeoff.
        constexpr usize kParallelRecordThreshold = 128;
    } // namespace

    Core::RendererResult Renderer::record_render_items_culled(RHI::RenderPassEncoder &pass,
                                                               span<const RenderItem> items,
                                                               const Frustum &frustum,
                                                               span<const RHI::Format> color_formats,
                                                               RHI::Format depth_format,
                                                               u64 frame_index,
                                                               const glm::mat4 &view_projection,
                                                               bool depth_only,
                                                               bool standard_depth_test,
                                                               const char *bundle_label,
                                                               bool shadow_map,
                                                               f32 shadow_depth_bias,
                                                               f32 shadow_slope_bias) {
        vector<const RenderItem *> visible;
        visible.reserve(items.size());
        for (const RenderItem &item : items) {
            if (render_item_visible(item, frustum)) {
                visible.push_back(&item);
            }
        }

        const u32 worker_count = Async::Scheduler::worker_count();
        // Shadow atlas views change viewport/scissor between light faces. Vulkan render bundles
        // (secondary command buffers) do not portably inherit that dynamic state from the primary
        // pass, so keep shadow-view recording on the primary encoder. Geometry passes use one fixed
        // full-frame viewport and retain the parallel bundle path.
        if (shadow_map || visible.size() < kParallelRecordThreshold || worker_count <= 1) {
            RenderItemBindingState binding_state{};
            for (const RenderItem *item : visible) {
                if (Core::RendererResult recorded = record_render_item(
                        pass, *item, color_formats, depth_format, frame_index, view_projection,
                        depth_only, binding_state, standard_depth_test, shadow_map,
                        shadow_depth_bias, shadow_slope_bias);
                    !recorded.has_value()) {
                    return recorded;
                }
            }
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }

        // Pre-warm every distinct material's bind groups on this thread first — see this function's
        // doc comment in RendererModule.hpp for why: it turns every worker thread's later
        // prepare_material_frame call (inside record_render_item) into a pure read of already-clean
        // state instead of a racy rebuild.
        {
            std::unordered_map<u64, bool> warmed;
            for (const RenderItem *item : visible) {
                if (!warmed.try_emplace(item->material.value, true).second) {
                    continue;
                }
                MaterialInstanceResource *material_resource = material_instance(item->material);
                if (material_resource == nullptr || material_resource->frames.empty()) {
                    continue;
                }
                const u32 frame_slot = static_cast<u32>(frame_index % material_resource->frames.size());
                if (auto prepared = prepare_material_frame(*material_resource, frame_slot); !prepared) {
                    return unexpected(prepared.error());
                }
            }
        }

        const RHI::RenderBundleDesc bundle_desc{
            .color_formats = color_formats,
            .depth_stencil_format = depth_format,
            .samples = RHI::SampleCount::X1,
            .view_mask = 0,
            .label = bundle_label,
        };

        const usize chunk_count = std::min<usize>(worker_count, visible.size());
        const usize chunk_size = (visible.size() + chunk_count - 1) / chunk_count;

        struct ChunkResult {
            Core::RendererResult status{};
            RHI::RenderBundleHandle bundle{};
        };
        vector<ChunkResult> results(chunk_count);
        vector<Async::TaskHandle<void>> tasks;
        tasks.reserve(chunk_count);
        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
            const usize begin = chunk * chunk_size;
            const usize end = std::min(visible.size(), begin + chunk_size);
            if (begin >= end) {
                continue;
            }
            tasks.push_back(Async::Scheduler::spawn([this, &visible, &results, chunk, begin, end, device, bundle_desc,
                                                      color_formats, depth_format, frame_index, view_projection,
                                                      depth_only, standard_depth_test, shadow_map,
                                                      shadow_depth_bias, shadow_slope_bias]() {
                auto encoder = device->create_render_bundle_encoder(bundle_desc);
                if (!encoder) {
                    results[chunk].status = unexpected(graphics_error_from_rhi(encoder.error(), "create render bundle encoder"));
                    return;
                }
                RenderItemBindingState binding_state{};
                for (usize i = begin; i < end; ++i) {
                    if (Core::RendererResult recorded = record_render_item(
                            **encoder, *visible[i], color_formats, depth_format, frame_index, view_projection,
                            depth_only, binding_state, standard_depth_test, shadow_map,
                            shadow_depth_bias, shadow_slope_bias);
                        !recorded.has_value()) {
                        results[chunk].status = recorded;
                        return;
                    }
                }
                auto finished = (*encoder)->finish();
                if (!finished) {
                    results[chunk].status = unexpected(graphics_error_from_rhi(finished.error(), "finish render bundle"));
                    return;
                }
                results[chunk].bundle = *finished;
            }));
        }
        for (const Async::TaskHandle<void> &task : tasks) {
            task.wait();
        }

        vector<RHI::RenderBundleHandle> bundles;
        bundles.reserve(chunk_count);
        Core::RendererResult first_error{};
        bool has_error = false;
        for (ChunkResult &result : results) {
            if (!result.status.has_value() && !has_error) {
                first_error = result.status;
                has_error = true;
            }
            if (result.bundle) {
                bundles.push_back(result.bundle);
            }
        }
        if (!bundles.empty()) {
            pass.execute_bundles(span<const RHI::RenderBundleHandle>{bundles.data(), bundles.size()});
        }
        for (RHI::RenderBundleHandle bundle : bundles) {
            device->destroy_render_bundle(bundle);
        }
        if (has_error) {
            return first_error;
        }
        return {};
    }

    Core::RendererResult Renderer::ensure_rhi_depth_resources(WindowSurfaceRecord &record) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (record.swapchain_extent.is_zero()) {
            return {};
        }
        if (record.depth_texture && record.depth_view) {
            return {};
        }

        auto depth_texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = record.depth_format,
            .extent = RHI::Extent3D{.width = record.swapchain_extent.width,
                                    .height = record.swapchain_extent.height,
                                    .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::DepthStencilAttachment,
            .label = "renderer depth texture",
        });
        if (!depth_texture) {
            return unexpected(graphics_error_from_rhi(depth_texture.error(), "create renderer depth texture"));
        }

        auto depth_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *depth_texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = "renderer depth view",
        });
        if (!depth_view) {
            device->destroy_texture(*depth_texture);
            return unexpected(graphics_error_from_rhi(depth_view.error(), "create renderer depth view"));
        }

        record.depth_texture = *depth_texture;
        record.depth_view = *depth_view;
        return {};
    }

    Core::RendererResult Renderer::recreate_rhi_swapchain(WindowSurfaceRecord &record, u64 frame_index,
                                                          optional<Core::Extent2D> known_extent) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (!record.rhi_surface) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an RHI swapchain without an RHI surface.");
        }
        if (!known_extent && record.window == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an RHI swapchain without a live window.");
        }

        const Core::Extent2D extent = known_extent ? *known_extent : framebuffer_extent(*record.window);
        if (extent.is_zero()) {
            record.rhi_swapchain_dirty = true;
            return {};
        }

        const RHI::SwapchainHandle old_swapchain = record.rhi_swapchain;
        const RHI::TextureHandle old_depth_texture = record.depth_texture;
        const RHI::TextureViewHandle old_depth_view = record.depth_view;

        RHI::SwapchainDesc swapchain_desc{
            .surface = record.rhi_surface,
            .width = extent.width,
            .height = extent.height,
            .format = static_cast<bool>(record.presentation.hdr_enabled) ? RHI::Format::RGB10A2Unorm : RHI::Format::BGRA8UnormSrgb,
            .color_space = static_cast<bool>(record.presentation.hdr_enabled) ? RHI::ColorSpace::Hdr10St2084 : RHI::ColorSpace::SrgbNonlinear,
            .present_mode = record.presentation.vsync
                                ? (record.presentation.present_mode == RHI::PresentMode::Immediate
                                       ? RHI::PresentMode::Mailbox
                                       : record.presentation.present_mode)
                                : RHI::PresentMode::Immediate,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::TransferDst,
            .composite_alpha = RHI::CompositeAlphaMode::Auto,
            .clipped = true,
            .image_count = record.presentation.swapchain_image_count != 0
                               ? record.presentation.swapchain_image_count
                               : record.desired_frames_in_flight + 1,
            .old_swapchain = old_swapchain,
            .label = "renderer swapchain",
        };
        auto swapchain = device->create_swapchain(swapchain_desc);
        if (!swapchain) {
            return unexpected(graphics_error_from_rhi(swapchain.error(), "create RHI swapchain"));
        }

        record.rhi_swapchain = *swapchain;
        record.depth_texture = {};
        record.depth_view = {};
        record.swapchain_extent = extent;
        record.rhi_swapchain_dirty = false;
        if (old_swapchain || old_depth_texture || old_depth_view) {
            FrameInFlight *retire_after = nullptr;
            if (!record.frames_in_flight.empty()) {
                const u64 retire_index = frame_index > 0 ? frame_index - 1 : frame_index;
                retire_after = &record.frames_in_flight[retire_index % record.frames_in_flight.size()];
            }

            if (retire_after != nullptr) {
                if (old_swapchain) {
                    retire_after->retired_swapchains.push_back(old_swapchain);
                }
                if (old_depth_view) {
                    retire_after->retired_presentation_texture_views.push_back(old_depth_view);
                }
                if (old_depth_texture) {
                    retire_after->retired_presentation_textures.push_back(old_depth_texture);
                }
            } else {
                // No frame ring exists yet, so nothing has acquired or presented through this swapchain.
                if (old_depth_view) {
                    device->destroy_texture_view(old_depth_view);
                }
                if (old_depth_texture) {
                    device->destroy_texture(old_depth_texture);
                }
                if (old_swapchain) {
                    device->destroy_swapchain(old_swapchain);
                }
            }
        }
        return ensure_rhi_depth_resources(record);
    }

    Core::RendererResult Renderer::render_frame_rhi(WindowSurfaceRecord &record,
                                                    const Core::FrameInput &frame,
                                                    FrameSubmission &submission) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }

        // N-buffered in-flight ring, keyed by frame_index so it tracks the material system's per-frame
        // UBO slot (frame_index % N). (Re)size on the first frame or after a capability change (device-loss
        // recovery clears the ring). Lives on the window's own record, not a Renderer-wide member, since
        // each window has its own swapchain and therefore its own frame-in-flight lifetime.
        const u32 frame_count = capabilities_.max_frames_in_flight == 0 ? 1u : capabilities_.max_frames_in_flight;
        if (record.frames_in_flight.size() != frame_count) {
            for (FrameInFlight &old_slot : record.frames_in_flight) {
                destroy_text_frame_resources(*device, old_slot.text_overlay_resources);
                destroy_frame_bloom_targets(old_slot);
                destroy_frame_composite_target(old_slot);
                destroy_frame_gpu_timing_target(old_slot);
                destroy_frame_shadow_targets(old_slot);
                destroy_frame_deferred_targets(old_slot);
            }
            record.frames_in_flight.assign(frame_count, FrameInFlight{});
        }
        FrameInFlight &slot = record.frames_in_flight[frame.frame_index % frame_count];

        // Collects this call's own CPU stage costs (wait fence, swapchain recreate/acquire, graph
        // execute, submit, present — whichever of those actually run this frame) so they can be
        // stashed on `slot` for the debug overlay to display next time this ring slot comes round —
        // see FrameCpuTimingTarget's doc comment for why "next time", not "this frame".
        vector<std::pair<string, f64>> current_frame_cpu_stage_timings_ms;

        // Backpressure — the one sanctioned per-frame CPU wait (plans/async-submission-model.md). Waits on
        // the *specific* frame that last used this ring slot (frame_count frames ago), never a full-device
        // stall, capping the CPU to frame_count frames ahead of the GPU. Once its fence signals, that
        // frame's command buffer / transient targets / bind groups are safe to reclaim and its material
        // UBO slot is free to overwrite. NOT safe to reclaim here: any swapchain/presentation texture
        // recreate_rhi_swapchain retired onto this slot — this fence only covers that frame's *command
        // buffer* submission, not the separate, driver-internal completion of its vkQueuePresentKHR
        // (validated: destroying here trips VUID-vkDestroySwapchainKHR-swapchain-01282, "swapchain
        // currently in use by VkQueue" — presents aren't fenced the way command buffers are without
        // VK_EXT_swapchain_maintenance1). Retired swapchains/textures accumulate on their slot until
        // maybe_flush_retired_swapchains() below periodically clears them with a real wait_idle().
        if (slot.submitted) {
            {
                ScopedRendererStageTimer timer{"wait in-flight frame fence", &current_frame_cpu_stage_timings_ms};
                if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true); !waited) {
                    return unexpected(graphics_error_from_rhi(waited.error(), "wait in-flight frame fence"));
                }
            }
            if (auto reset = device->reset_fences(span<const RHI::FenceHandle>{&slot.fence, 1}); !reset) {
                return unexpected(graphics_error_from_rhi(reset.error(), "reset in-flight frame fence"));
            }
            reclaim_frame_slot(slot, false);
            slot.submitted = false;
        }

        // GPU pass timing readback — the fence wait just above (when this slot had a prior submission)
        // is the earliest point the GPU is guaranteed to have written every timestamp RenderGraph::
        // execute() queued for it last time this ring slot was used (see FrameGpuTimingTarget's doc
        // comment). Read once here rather than at the point the graph executes further down, since
        // this frame's own about-to-be-recorded timestamps land in the SAME query set slots.
        vector<std::pair<string, f64>> gpu_pass_timings_ms;
        if (slot.gpu_timing.has_pending_results) {
            const f32 period_ns = device->limits().timestamp_period_ns;
            if (period_ns > 0.0f && !slot.gpu_timing.pending.empty()) {
                // Only the slots RenderGraph::execute() actually reset+wrote *this specific prior
                // frame* (2 per pass it recorded that frame) are valid to read — not the query set's
                // full allocated capacity, which can exceed that (headroom from ensure_frame_gpu_
                // timing_target's resize policy, or simply a larger pass count from an earlier frame
                // that grew capacity but isn't this frame's). Reading an unwritten slot is a real
                // "query not reset" validation error, not just wasted work.
                u32 used_query_count = 0;
                for (const RenderGraph::GpuPassTiming &timing : slot.gpu_timing.pending) {
                    used_query_count = std::max(used_query_count, timing.begin_query_index + 1);
                    used_query_count = std::max(used_query_count, timing.end_query_index + 1);
                }
                vector<u64> raw_ticks(used_query_count, 0);
                auto read = device->get_query_set_results(
                    slot.gpu_timing.query_set, 0, used_query_count,
                    std::as_writable_bytes(span<u64>{raw_ticks.data(), raw_ticks.size()}), sizeof(u64),
                    RHI::QueryResultFlags::Result64Bit | RHI::QueryResultFlags::Wait);
                if (read.has_value()) {
                    std::unordered_map<string, f64> totals_ms;
                    for (const RenderGraph::GpuPassTiming &timing : slot.gpu_timing.pending) {
                        if (timing.begin_query_index >= raw_ticks.size() || timing.end_query_index >= raw_ticks.size()) {
                            continue;
                        }
                        const u64 begin_ticks = raw_ticks[timing.begin_query_index];
                        const u64 end_ticks = raw_ticks[timing.end_query_index];
                        if (end_ticks <= begin_ticks) {
                            continue;
                        }
                        const f64 ms = static_cast<f64>(end_ticks - begin_ticks) * static_cast<f64>(period_ns) / 1.0e6;
                        totals_ms[render_graph_pass_timing_category(timing.label)] += ms;
                    }
                    gpu_pass_timings_ms.assign(totals_ms.begin(), totals_ms.end());
                    std::sort(gpu_pass_timings_ms.begin(), gpu_pass_timings_ms.end(),
                             [](const auto &a, const auto &b) { return a.second > b.second; });
                }
            }
            slot.gpu_timing.has_pending_results = false;
        }

        // CPU pass/stage timing readback — no query/fence dependency (it's wall-clock CPU time,
        // ready the instant last frame's render_frame_rhi call returned), but still read back here
        // rather than computed fresh below: this frame's debug-overlay text is built (see further
        // down) before this frame's own RenderGraph::execute() call has run, so "this frame's own
        // numbers" don't exist yet either way — same one-frame-stale contract as GPU timing above,
        // just for a different reason.
        vector<std::pair<string, f64>> cpu_pass_timings_ms;
        vector<std::pair<string, f64>> cpu_stage_timings_ms;
        if (slot.cpu_timing.has_pending_results) {
            std::unordered_map<string, f64> totals_ms;
            for (const RenderGraph::CpuPassTiming &timing : slot.cpu_timing.pass_timings) {
                totals_ms[render_graph_pass_timing_category(timing.label)] += timing.duration_ms;
            }
            cpu_pass_timings_ms.assign(totals_ms.begin(), totals_ms.end());
            std::sort(cpu_pass_timings_ms.begin(), cpu_pass_timings_ms.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
            cpu_stage_timings_ms = slot.cpu_timing.stage_timings;
            slot.cpu_timing.has_pending_results = false;
        }

        if (!slot.fence) {
            auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer frame fence"});
            if (!fence) {
                return unexpected(graphics_error_from_rhi(fence.error(), "create RHI frame fence"));
            }
            slot.fence = *fence;
        }

        if (Core::RendererResult resources = ensure_rhi_presentation_resources(record); !resources.has_value()) {
            return resources;
        }

        // Framebuffer size comes from FrameInput (already fresh from whichever thread owns the window
        // this tick — see Application::render_managed_window), never by re-querying the Window object
        // here: this runs on the render thread, which must never touch a Window directly once a separate
        // window-event thread may be pumping it concurrently.
        const Core::Extent2D extent{frame.framebuffer_width, frame.framebuffer_height};
        if (extent.is_zero()) {
            return {};
        }
        const bool size_changed = extent.width != record.swapchain_extent.width ||
            extent.height != record.swapchain_extent.height;
        const bool should_recreate = record.rhi_swapchain_dirty || size_changed;
        if (should_recreate) {
            // Rebuild immediately on every size change, every frame — no debounce/rate-limit. A
            // gate here (wait for the extent to stabilize, or cap rebuild frequency) trades away
            // exactly the "surface visibly tracks the live drag" behavior we want: on Linux/Wayland
            // especially, a continuous drag delivers a new extent essentially every frame, and any
            // gate either never fires (frozen at the pre-drag size until the drag pauses) or caps
            // the resize to a fixed cadence — both look laggy compared to just recreating every time.
            {
                ScopedRendererStageTimer timer{"recreate swapchain", &current_frame_cpu_stage_timings_ms};
                if (Core::RendererResult recreated = recreate_rhi_swapchain(record, frame.frame_index, extent); !recreated.has_value()) {
                    return recreated;
                }
            }
            // Bounded safety net only — see maybe_flush_retired_swapchains' declaration comment.
            // Doesn't fire on an ordinary resize (one retired swapchain); only kicks in if a single
            // continuous drag runs long enough to pile up several without ever pausing.
            maybe_flush_retired_swapchains(record, false);
        } else {
            // Not resizing this frame: any swapchain retired by an *earlier* resize is cleaned up
            // right now rather than left to linger — an extra live swapchain is dead weight the WSI
            // carries on every subsequent acquire/present until it's gone, so "no active resize
            // happening" is the first safe, free opportunity to pay the one wait_idle() that proves
            // it's actually safe to destroy (see reclaim_frame_slot's comment on why we can't do
            // this off a single frame-fence wait). A no-op most frames — it only does anything when
            // there's an actual backlog.
            maybe_flush_retired_swapchains(record, true);
        }
        if (!record.rhi_swapchain) {
            return {};
        }
        const Core::Extent2D presentation_extent = record.swapchain_extent;
        const f32 resolution_scale = std::clamp(submission.render_graph.resolution_scale, 0.1f, 2.0f);
        const Core::Extent2D render_extent{
            .width = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(presentation_extent.width) * resolution_scale))),
            .height = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(presentation_extent.height) * resolution_scale))),
        };
        if (Core::RendererResult depth_resources = ensure_rhi_depth_resources(record); !depth_resources.has_value()) {
            return depth_resources;
        }
        {
            ScopedRendererStageTimer timer{"prepare scene GPU data", &current_frame_cpu_stage_timings_ms};
            if (Core::RendererResult scene_gpu_data = prepare_scene_gpu_data(frame.frame_index, submission); !scene_gpu_data.has_value()) {
                return scene_gpu_data;
            }
        }

        // GPU-driven instanced batches: contiguous same-(mesh, material) runs of submission.draws
        // large enough to be worth one compute-culled indirect draw instead of many individual
        // per-item ones — see detect_instanced_batches's doc comment (RendererModule.hpp) and
        // Shaders/gpu_instance_cull.slang's header comment for the full design. Scoped to the
        // deferred gbuffer geometry pass only for now: batched instances still go through the
        // ordinary z-prepass per-item path below unfiltered (their own pipeline variant uses a
        // standard depth test/write instead of relying on a prior z-prepass write — see
        // instanced_pipeline_for's doc comment — so skipping them there is harmless, just leaves
        // some overdraw-elimination on the table for this specific batch).
        const vector<InstancedBatch> instanced_batches =
            submission.render_graph.render_scene ? detect_instanced_batches(submission.draws) : vector<InstancedBatch>{};
        const u32 scene_frame_count = capabilities_.max_frames_in_flight == 0 ? 1u : capabilities_.max_frames_in_flight;
        SceneFrameGpuResources &instance_cull_resources = scene_frame_resources_[frame.frame_index % scene_frame_count];
        if (!instanced_batches.empty()) {
            ScopedRendererStageTimer timer{"prepare instance cull GPU data", &current_frame_cpu_stage_timings_ms};
            if (Core::RendererResult prepared = prepare_instance_cull_gpu_data(instanced_batches, instance_cull_resources);
                !prepared.has_value()) {
                return prepared;
            }
        }
        // The gbuffer pass draws every batched instance once via its own indirect draw (below); the
        // per-item path must skip them so they aren't drawn twice. The z-prepass remains unaffected
        // and consumes the full, unfiltered submission.draws — see the comment above.
        vector<RenderItem> gbuffer_individual_draws_storage;
        span<const RenderItem> gbuffer_draws = submission.draws;
        if (!instanced_batches.empty()) {
            gbuffer_individual_draws_storage.reserve(submission.draws.size());
            usize batch_cursor = 0;
            for (usize i = 0; i < submission.draws.size(); ++i) {
                if (batch_cursor < instanced_batches.size() &&
                    i >= instanced_batches[batch_cursor].first_object_index &&
                    i < static_cast<usize>(instanced_batches[batch_cursor].first_object_index) + instanced_batches[batch_cursor].instance_count) {
                    if (i + 1 == static_cast<usize>(instanced_batches[batch_cursor].first_object_index) + instanced_batches[batch_cursor].instance_count) {
                        ++batch_cursor;
                    }
                    continue;
                }
                gbuffer_individual_draws_storage.push_back(submission.draws[i]);
            }
            gbuffer_draws = gbuffer_individual_draws_storage;
        }

        if (Core::RendererResult deferred_targets = ensure_frame_deferred_targets(slot, render_extent, submission.deferred_formats); !deferred_targets.has_value()) {
            return deferred_targets;
        }
        PreparedShadowFrame shadow_frame{};
        if (submission.render_graph.render_scene) {
            const u32 requested_shadow_atlas = submission.render_graph.shadows
                                                   ? submission.render_graph.shadow_atlas_size
                                                   : 0u;
            if (Core::RendererResult shadow_targets = ensure_frame_shadow_targets(slot, requested_shadow_atlas);
                !shadow_targets.has_value()) {
                return shadow_targets;
            }
            if (Core::RendererResult shadow_resources = ensure_shadow_lighting_resources();
                !shadow_resources.has_value()) {
                return shadow_resources;
            }
            if (auto lighting_pipeline = shadow_lighting_pipeline_for(submission.deferred_formats.scene_color);
                !lighting_pipeline) {
                return unexpected(lighting_pipeline.error());
            }
            if (Core::RendererResult shadow_prepared = prepare_shadow_frame(submission, slot.shadow_targets,
                                                                            shadow_frame);
                !shadow_prepared.has_value()) {
                return shadow_prepared;
            }
        }
        // Pre-warm fullscreen post-process shaders/pipelines before recording so render-pass callbacks only
        // mint bind groups + draw — never compile shaders or build pipelines mid command-buffer recording.
        constexpr RHI::Format bloom_format = RHI::Format::RG11B10Float;
        const bool bloom_active = submission.render_graph.bloom && submission.render_graph.bloom_intensity > 0.0f;
        if (bloom_active) {
            if (Core::RendererResult bloom_ready = ensure_bloom_resources(bloom_format); !bloom_ready.has_value()) {
                return bloom_ready;
            }
            if (Core::RendererResult bloom_targets = ensure_frame_bloom_targets(slot, render_extent, submission.render_graph.bloom_max_levels); !bloom_targets.has_value()) {
                return bloom_targets;
            }
            if (Core::RendererResult composite_ready = ensure_bloom_composite_resources(); !composite_ready.has_value()) {
                return composite_ready;
            }
            if (auto composite_pipeline = bloom_composite_pipeline_for(submission.deferred_formats.scene_color); !composite_pipeline) {
                return unexpected(composite_pipeline.error());
            }
            if (Core::RendererResult composite_target = ensure_frame_composite_target(slot, render_extent, submission.deferred_formats.scene_color); !composite_target.has_value()) {
                return composite_target;
            }
        }
        for (const CustomPostProcessEffect &effect : submission.render_graph.custom_post_processes) {
            if (Core::RendererResult custom_ready = ensure_custom_post_process(effect, submission.deferred_formats.scene_color); !custom_ready.has_value()) {
                return custom_ready;
            }
        }
        if (Core::RendererResult tonemap_ready = ensure_tonemap_resources(); !tonemap_ready.has_value()) {
            return tonemap_ready;
        }
        if (auto tonemap_pipeline = tonemap_pipeline_for(RHI::Format::BGRA8UnormSrgb); !tonemap_pipeline) {
            return unexpected(tonemap_pipeline.error());
        }

        auto acquired = [&]() {
            ScopedRendererStageTimer timer{"acquire swapchain texture", &current_frame_cpu_stage_timings_ms};
            return device->acquire_next_texture(record.rhi_swapchain);
        }();
        if (!acquired) {
            if (acquired.error().code == RHI::RhiErrorCode::NotReady) {
                return {};
            }
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

        vector<TextDrawBatch> text_overlay_batches;
        if (submission.render_graph.debug_overlay) {
            // The large-text path still virtualizes, caches shaping/layout, and avoids redundant
            // instance uploads; only the two changing counter lines need reshaping each frame.
            const f32 overlay_fps = frame.delta_seconds > 0.0 ? static_cast<f32>(1.0 / frame.delta_seconds) : 0.0f;
            const optional<Core::GpuInfo> overlay_gpu_info = gpu_info();
            vector<UString> overlay_lines{
                submission.debug_label.empty() ? UString{"Scene"_ustr} : submission.debug_label,
                std::format("Renderables: {}", submission.draws.size()),
                std::format("Camera: ({:.2f}, {:.2f}, {:.2f})", submission.camera.world_position.x,
                            submission.camera.world_position.y, submission.camera.world_position.z),
                std::format("Resolution: {}x{} (scene {}x{}, {:.0f}%)",
                            presentation_extent.width, presentation_extent.height,
                            render_extent.width, render_extent.height, resolution_scale * 100.0f),
                std::format("GPU: {}", overlay_gpu_info ? overlay_gpu_info->name : string{"unknown"}),
                std::format("FPS: {:.1f} ({:.2f} ms)", overlay_fps, frame.delta_seconds * 1000.0),
                std::format("Frame: {}", frame.frame_index),
            };
            // GPU pass timing breakdown — one frame stale (this frame's own timestamps aren't
            // available until its fence signals; see gpu_pass_timings_ms's own comment above), same
            // one-frame-behind tradeoff every other per-frame stat here already accepts implicitly.
            if (!gpu_pass_timings_ms.empty()) {
                f64 gpu_total_ms = 0.0;
                for (const auto &[category, ms] : gpu_pass_timings_ms) {
                    gpu_total_ms += ms;
                }
                overlay_lines.push_back(std::format("GPU total: {:.2f} ms", gpu_total_ms));
                for (const auto &[category, ms] : gpu_pass_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", category, ms));
                }
            }
            // CPU stage timing breakdown — the coarse top-level stages (extraction/sort in
            // render_frame, then render_frame_rhi's own fence-wait/graph-execute/submit/present
            // stages). One frame stale, same reason as the GPU numbers above.
            if (!cpu_stage_timings_ms.empty()) {
                f64 cpu_stage_total_ms = 0.0;
                for (const auto &[stage, ms] : cpu_stage_timings_ms) {
                    cpu_stage_total_ms += ms;
                }
                overlay_lines.push_back(std::format("CPU frame total: {:.2f} ms", cpu_stage_total_ms));
                for (const auto &[stage, ms] : cpu_stage_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", stage, ms));
                }
            }
            // CPU pass-recording breakdown — how long the CPU spent recording each RenderGraph pass
            // (barrier insertion + record_render_items_culled/etc.), the direct counterpart to the
            // GPU breakdown above. This is what makes parallel-vs-serial recording wins (and, once
            // GPU-driven culling lands, the drop from many CPU draw calls to one compute-culled
            // indirect draw) visible per pass instead of only as one lump "execute render graph"
            // stage total.
            if (!cpu_pass_timings_ms.empty()) {
                f64 cpu_pass_total_ms = 0.0;
                for (const auto &[category, ms] : cpu_pass_timings_ms) {
                    cpu_pass_total_ms += ms;
                }
                overlay_lines.push_back(std::format("CPU pass recording total: {:.2f} ms", cpu_pass_total_ms));
                for (const auto &[category, ms] : cpu_pass_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", category, ms));
                }
            }
            // The encoder's unique_ptr cleans up the abandoned recording automatically on this early
            // return (nothing has been submitted yet, so there's nothing else to unwind).
            if (Core::RendererResult text_prepared =
                    prepare_text_overlay(**encoder, span<const UString>{overlay_lines.data(), overlay_lines.size()},
                                         glm::vec2{10.0f, 10.0f},
                                         glm::vec2{static_cast<f32>(presentation_extent.width), static_cast<f32>(presentation_extent.height)},
                                         slot.text_overlay_resources,
                                         submission.transient_buffers, submission.retired_text_atlas_resources,
                                         text_overlay_batches);
                !text_prepared.has_value()) {
                return text_prepared;
            }
        }

        RenderGraph graph;
        // Not a ScopedRendererStageTimer: this stage spans the whole pass-declaration section below
        // (every add_render_pass/add_compute_pass/set_execute call, down to just before "execute
        // render graph" starts), which is too much code to wrap in one extra brace level without
        // touching every line in between. Measured by hand instead — see its matching read-out
        // right before the "execute render graph" scope.
        const steady_clock::time_point declare_graph_start = steady_clock::now();
        const glm::vec4 background{
            submission.render_graph.background_color.r * submission.render_graph.background_intensity,
            submission.render_graph.background_color.g * submission.render_graph.background_intensity,
            submission.render_graph.background_color.b * submission.render_graph.background_intensity,
            submission.render_graph.background_color.a,
        };
        const RenderGraphTextureHandle swapchain_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = texture.texture,
            .default_view = texture.view,
            .format = RHI::Format::BGRA8UnormSrgb,
            .extent = RHI::Extent3D{.width = presentation_extent.width, .height = presentation_extent.height, .depth_or_layers = 1},
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::Present,
            .final_stage = RHI::PipelineStage::None,
            .final_access = RHI::AccessFlags::None,
            .label = "swapchain color",
        });
        const RHI::Extent3D frame_extent{.width = render_extent.width, .height = render_extent.height, .depth_or_layers = 1};
        const RenderGraphTextureHandle gbuffer_albedo = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.gbuffer_albedo,
            .default_view = slot.deferred_targets.gbuffer_albedo_view,
            .format = submission.deferred_formats.albedo,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer albedo",
        });
        const RenderGraphTextureHandle gbuffer_normal = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.gbuffer_normal,
            .default_view = slot.deferred_targets.gbuffer_normal_view,
            .format = submission.deferred_formats.normal,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer normal",
        });
        const RenderGraphTextureHandle gbuffer_material = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.gbuffer_material,
            .default_view = slot.deferred_targets.gbuffer_material_view,
            .format = submission.deferred_formats.material,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer material",
        });
        // HDR scene-color target consumed by gizmos and post-processing.
        const RenderGraphTextureHandle scene_color = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.scene_color,
            .default_view = slot.deferred_targets.scene_color_view,
            .format = submission.deferred_formats.scene_color,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "scene color (HDR)",
        });
        const RenderGraphTextureHandle depth_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.depth,
            .default_view = slot.deferred_targets.depth_view,
            .format = submission.deferred_formats.depth,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::DepthStencilAttachment,
            .final_stage = RHI::PipelineStage::LateFragmentTests,
            .final_access = RHI::AccessFlags::DepthStencilAttachmentWrite,
            .label = "deferred depth",
        });
        RenderGraphTextureHandle shadow_atlas{};
        if (shadow_frame.atlas_used) {
            shadow_atlas = graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = slot.shadow_targets.atlas,
                .default_view = slot.shadow_targets.atlas_view,
                .format = slot.shadow_targets.format,
                .extent = RHI::Extent3D{.width = slot.shadow_targets.atlas_size,
                                        .height = slot.shadow_targets.atlas_size,
                                        .depth_or_layers = 1},
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .final_layout = RHI::TextureLayout::ShaderReadOnly,
                .final_stage = RHI::PipelineStage::FragmentShader,
                .final_access = RHI::AccessFlags::ShaderRead,
                .label = "raster shadow atlas",
            });
        }

        // Shared by "z prepass" and "deferred gbuffer geometry" below — both draw the same
        // submission.draws against the same camera view, so items outside the camera frustum never
        // need a draw call issued for either pass.
        const Frustum camera_frustum = frustum_from_view_projection(submission.view_projection);

        if (!instanced_batches.empty()) {
            graph.add_compute_pass("gpu instance cull")
                .set_execute([this, &submission, &instanced_batches, &instance_cull_resources](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    RHI::ComputePassEncoder &pass = context.compute_pass();
                    if (Core::RendererResult culled = record_instance_cull(
                            pass, instanced_batches, submission.view_projection, instance_cull_resources,
                            submission.transient_bind_groups);
                        !culled.has_value()) {
                        return culled;
                    }
                    // Manual barrier: RenderGraphComputePassBuilder only auto-tracks texture hazards
                    // (add_sampled_texture/add_storage_texture), not buffers — see this pass's
                    // declaration and record_instanced_batches's doc comment. Safe to record here
                    // (not "inside an active render pass"): unlike a graphics render pass, this RHI's
                    // compute-pass encoder is a lightweight logical wrapper, not a real GPU scope
                    // (VulkanRhiDeviceBridge::begin_compute_pass records nothing at begin/end), so a
                    // barrier on the underlying command encoder here is ordered correctly.
                    const array<RHI::BufferBarrier, 2> buffer_barriers{
                        RHI::BufferBarrier{
                            .buffer = instance_cull_resources.indirect_commands_buffer,
                            .src_stage = RHI::PipelineStage::ComputeShader,
                            .src_access = RHI::AccessFlags::ShaderWrite,
                            .dst_stage = RHI::PipelineStage::DrawIndirect,
                            .dst_access = RHI::AccessFlags::IndirectCommandRead,
                        },
                        RHI::BufferBarrier{
                            .buffer = instance_cull_resources.compacted_indices_buffer,
                            .src_stage = RHI::PipelineStage::ComputeShader,
                            .src_access = RHI::AccessFlags::ShaderWrite,
                            .dst_stage = RHI::PipelineStage::VertexShader,
                            .dst_access = RHI::AccessFlags::ShaderRead,
                        },
                    };
                    context.command_encoder().barrier(span<const RHI::GlobalBarrier>{},
                                                      span<const RHI::BufferBarrier>{buffer_barriers.data(), buffer_barriers.size()},
                                                      span<const RHI::TextureBarrier>{});
                    return {};
                });
        }

        if (submission.render_graph.render_scene) {
            if (shadow_frame.atlas_used) {
                graph.add_render_pass("raster shadow atlas")
                    .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                        .texture = shadow_atlas,
                        .depth_load_op = RHI::LoadOp::Clear,
                        .depth_store_op = RHI::StoreOp::Store,
                        .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                    })
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0,
                                                 .width = slot.shadow_targets.atlas_size,
                                                 .height = slot.shadow_targets.atlas_size})
                    .set_execute([this, &submission, &shadow_frame, &slot, frame](
                                     RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        const f32 shadow_depth_bias = std::isfinite(submission.render_graph.shadow_depth_bias)
                                                          ? std::max(submission.render_graph.shadow_depth_bias, 0.0f)
                                                          : 0.75f;
                        const f32 shadow_slope_bias = std::isfinite(submission.render_graph.shadow_slope_bias)
                                                          ? std::max(submission.render_graph.shadow_slope_bias, 0.0f)
                                                          : 1.0f;
                        for (usize view_index = 0; view_index < shadow_frame.render_views.size(); ++view_index) {
                            const ShadowRenderView &shadow_view = shadow_frame.render_views[view_index];
                            pass.set_viewport(RHI::Viewport{
                                .x = static_cast<f32>(shadow_view.viewport.x),
                                .y = static_cast<f32>(shadow_view.viewport.y),
                                .width = static_cast<f32>(shadow_view.viewport.width),
                                .height = static_cast<f32>(shadow_view.viewport.height),
                                .min_depth = 0.0f,
                                .max_depth = 1.0f,
                            });
                            pass.set_scissor(shadow_view.viewport);
                            if (Core::RendererResult recorded = record_render_items_culled(
                                    pass, submission.draws, shadow_view.frustum, span<const RHI::Format>{},
                                    slot.shadow_targets.format, frame.frame_index,
                                    shadow_view.view_projection, /*depth_only=*/true,
                                    /*standard_depth_test=*/false, "raster shadow casters",
                                    /*shadow_map=*/true, shadow_depth_bias, shadow_slope_bias);
                                !recorded.has_value()) {
                                return recorded;
                            }
                        }
                        return {};
                    });
            }

            // Z prepass: writes real depth for every surviving (alpha-tested-or-not) fragment before
            // any color shading happens, so "deferred gbuffer geometry" below can require an exact
            // depth match instead of writing depth itself — a fragment that isn't the visible surface
            // never runs full PBR shading, eliminating occluded-fragment overdraw cost. See
            // Renderer::depth_only_pipeline_for's doc comment.
            graph.add_render_pass("z prepass")
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    .depth_load_op = RHI::LoadOp::Clear,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                .set_execute([this, &submission, render_extent, frame, camera_frustum](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.width),
                        .height = static_cast<f32>(render_extent.height),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                    return record_render_items_culled(pass, submission.draws, camera_frustum,
                                                       span<const RHI::Format>{}, submission.deferred_formats.depth,
                                                       frame.frame_index, submission.view_projection,
                                                       /*depth_only=*/true, /*standard_depth_test=*/false, "z prepass");
                });

            graph.add_render_pass("deferred gbuffer geometry")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = gbuffer_albedo,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                })
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = gbuffer_normal,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.5f, 0.5f, 0.0f, 0.0f},
                })
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = gbuffer_material,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
                })
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    // The Z prepass above already cleared+wrote this frame's depth — load, don't clear.
                    .depth_load_op = RHI::LoadOp::Load,
                    .depth_store_op = RHI::StoreOp::Store,
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                .set_execute([this, &submission, render_extent, frame, camera_frustum, gbuffer_draws, &instanced_batches,
                             &instance_cull_resources](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.width),
                        .height = static_cast<f32>(render_extent.height),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                    const array<RHI::Format, 3> gbuffer_formats{
                        submission.deferred_formats.albedo,
                        submission.deferred_formats.normal,
                        submission.deferred_formats.material,
                    };
                    const span<const RHI::Format> gbuffer_formats_span{gbuffer_formats.data(), gbuffer_formats.size()};
                    if (Core::RendererResult recorded = record_render_items_culled(
                            pass, gbuffer_draws, camera_frustum, gbuffer_formats_span, submission.deferred_formats.depth,
                            frame.frame_index, submission.view_projection, /*depth_only=*/false,
                            /*standard_depth_test=*/false, "deferred gbuffer geometry");
                        !recorded.has_value()) {
                        return recorded;
                    }
                    if (!instanced_batches.empty()) {
                        if (Core::RendererResult recorded_instanced = record_instanced_batches(
                                pass, instanced_batches, gbuffer_formats_span, submission.deferred_formats.depth,
                                frame.frame_index, submission.view_projection, instance_cull_resources,
                                submission.transient_bind_groups);
                            !recorded_instanced.has_value()) {
                            return recorded_instanced;
                        }
                    }
                    return {};
                });
        }

        if (submission.render_graph.render_scene) {
            RenderGraphRenderPassBuilder &lighting_pass = graph.add_render_pass("deferred shadow lighting");
            lighting_pass.add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            });
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_albedo});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_normal});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_material});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = depth_texture});
            if (shadow_frame.atlas_used) {
                lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = shadow_atlas});
            }
            lighting_pass
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, depth_texture, shadow_atlas, &shadow_frame](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.width),
                        .height = static_cast<f32>(render_extent.height),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0,
                                                 .width = render_extent.width, .height = render_extent.height});
                    const RHI::TextureViewHandle atlas_view = shadow_frame.atlas_used
                        ? context.texture(shadow_atlas).default_view
                        : context.texture(depth_texture).default_view;
                    return record_shadow_lighting(
                        pass,
                        context.texture(gbuffer_albedo).default_view,
                        context.texture(gbuffer_normal).default_view,
                        context.texture(gbuffer_material).default_view,
                        context.texture(depth_texture).default_view,
                        atlas_view,
                        slot.shadow_targets.lighting_buffer,
                        submission.deferred_formats.scene_color,
                        submission.transient_bind_groups);
                });
        } else {
            // Overlay-only views still need a defined HDR source for gizmos and post-processing.
            graph.add_render_pass("scene background")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = scene_color,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{background.r, background.g, background.b, background.a},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width,
                                             .height = render_extent.height});
        }

        // Always-on debug markers (e.g. light-position icospheres, Shaders/geometry_color.slang).
        // A separate, single-color-target forward pass rather than folding gizmos into the deferred
        // G-buffer pass: that pass's pipelines are built for a 3-target GBufferOutput, and a debug
        // marker's pass-through shader only ever writes one SV_Target. When scene geometry ran, load
        // its depth so occluded gizmos stay hidden; otherwise clear depth for a defined overlay-only pass.
        if (!submission.gizmo_draws.empty()) {
            const array<RHI::Format, 1> gizmo_color_formats{submission.deferred_formats.scene_color};
            graph.add_render_pass("light gizmos")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = scene_color,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    .depth_load_op = submission.render_graph.render_scene ? RHI::LoadOp::Load : RHI::LoadOp::Clear,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                .set_execute([this, &submission, render_extent, frame, gizmo_color_formats](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.width),
                        .height = static_cast<f32>(render_extent.height),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                    RenderItemBindingState binding_state{};
                    for (const RenderItem &item : submission.gizmo_draws) {
                        if (Core::RendererResult recorded = record_render_item(
                                pass, item, span<const RHI::Format>{gizmo_color_formats.data(), gizmo_color_formats.size()},
                                submission.deferred_formats.depth, frame.frame_index, submission.view_projection,
                                /*depth_only=*/false, binding_state, /*standard_depth_test=*/true);
                            !recorded.has_value()) {
                            return recorded;
                        }
                    }
                    return {};
                });
        }

        // Applies every custom effect whose declared stage matches `stage`, in original declaration
        // order, chaining source -> new transient target -> ... Reused for both BeforeBloom and
        // AfterBloomBeforeToneMap so the two stages are identical machinery, just different insertion
        // points around bloom.
        const auto apply_custom_post_process_stage = [this, &graph, &submission, render_extent, frame_extent](
            RenderGraphTextureHandle source, PostProcessStage stage) -> RenderGraphTextureHandle {
            for (usize effect_index = 0; effect_index < submission.render_graph.custom_post_processes.size(); ++effect_index) {
                if (submission.render_graph.custom_post_processes[effect_index].stage != stage) {
                    continue;
                }
                const RenderGraphTextureHandle from = source;
                const RenderGraphTextureHandle to = graph.create_texture(RenderGraphTextureDesc{
                    .format = submission.deferred_formats.scene_color,
                    .extent = frame_extent,
                    .label = "custom HDR post-process target",
                });
                graph.add_render_pass("custom HDR post-process")
                    .add_color_attachment(RenderGraphColorAttachmentDesc{
                        .texture = to,
                        .load_op = RHI::LoadOp::DontCare,
                        .store_op = RHI::StoreOp::Store,
                    })
                    .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = from})
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                    .set_execute([this, &submission, from, effect_index, render_extent](RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(render_extent.width), .height = static_cast<f32>(render_extent.height), .min_depth = 0.0f, .max_depth = 1.0f});
                        pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                        return record_custom_post_process(pass, context.texture(from).default_view,
                                                          submission.deferred_formats.scene_color,
                                                          submission.render_graph.custom_post_processes[effect_index],
                                                          submission.transient_bind_groups);
                    });
                source = to;
            }
            return source;
        };

        // BeforeBloom effects run first: their result is both bloom's actual source and, when bloom
        // is inactive, the direct input to the AfterBloomBeforeToneMap chain below.
        const RenderGraphTextureHandle post_process_source =
            apply_custom_post_process_stage(scene_color, PostProcessStage::BeforeBloom);

        RenderGraphTextureHandle after_bloom_source = post_process_source;
        if (bloom_active) {
            const vector<Core::Extent2D> &bloom_extents = slot.bloom_targets.extents;
            const Core::Extent2D bloom_base_extent = bloom_extents.front();
            const RenderGraphTextureHandle bloom_chain = graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = slot.bloom_targets.texture,
                .default_view = slot.bloom_targets.views.front(),
                .format = bloom_format,
                .extent = RHI::Extent3D{.width = bloom_base_extent.width, .height = bloom_base_extent.height, .depth_or_layers = 1},
                .mip_levels = static_cast<u32>(bloom_extents.size()),
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "persistent bloom mip chain",
            });

            for (usize level = 0; level < bloom_extents.size(); ++level) {
                const RenderGraphTextureHandle source = level == 0 ? post_process_source : bloom_chain;
                // Level 0's view/bind group can't be precomputed here like every other level: they're
                // resolved/created inside the lambda below instead (see its comment).
                const RHI::TextureViewHandle mip_source_view = level == 0
                    ? RHI::TextureViewHandle{}
                    : slot.bloom_targets.views[level - 1];
                const RHI::TextureSubresourceRange source_subresources = level == 0
                    ? RHI::TextureSubresourceRange{}
                    : RHI::TextureSubresourceRange{.base_mip_level = static_cast<u32>(level - 1), .mip_level_count = 1};
                const RHI::TextureSubresourceRange destination_subresources{
                    .base_mip_level = static_cast<u32>(level), .mip_level_count = 1,
                };
                const Core::Extent2D source_extent = level == 0 ? render_extent : bloom_extents[level - 1];
                const Core::Extent2D destination_extent = bloom_extents[level];
                const RHI::BindGroupHandle cached_bind_group = slot.bloom_targets.downsample_bind_groups[level];
                graph.add_render_pass("bloom downsample")
                    .add_color_attachment(RenderGraphColorAttachmentDesc{
                        .texture = bloom_chain,
                        .view = slot.bloom_targets.views[level],
                        .subresources = destination_subresources,
                        .load_op = RHI::LoadOp::DontCare,
                        .store_op = RHI::StoreOp::Store,
                    })
                    .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                        .texture = source, .subresources = source_subresources,
                    })
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.width, .height = destination_extent.height})
                    .set_execute([this, &submission, post_process_source, mip_source_view, source_extent, destination_extent, level, cached_bind_group](RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(destination_extent.width), .height = static_cast<f32>(destination_extent.height), .min_depth = 0.0f, .max_depth = 1.0f});
                        pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.width, .height = destination_extent.height});
                        RHI::TextureViewHandle source_view = mip_source_view;
                        RHI::BindGroupHandle bind_group = cached_bind_group;
                        if (level == 0) {
                            // post_process_source may be a fresh transient texture every frame (whenever
                            // there's a BeforeBloom effect), so unlike every other bloom mip its view
                            // isn't known until the graph resolves it here, and FrameBloomTargets' cached
                            // persistent bind group (built for the base scene-color view) would be stale.
                            // Resolve and mint both fresh, retiring the bind group with this frame.
                            source_view = context.texture(post_process_source).default_view;
                            auto dynamic_bind_group = create_bloom_source_bind_group(source_view);
                            if (!dynamic_bind_group.has_value()) {
                                return unexpected(dynamic_bind_group.error());
                            }
                            submission.transient_bind_groups.push_back(*dynamic_bind_group);
                            bind_group = *dynamic_bind_group;
                        }
                        return record_bloom_downsample(pass, source_view,
                            glm::vec2{1.0f / static_cast<f32>(source_extent.width), 1.0f / static_cast<f32>(source_extent.height)},
                            submission.render_graph, level == 0, bind_group);
                    });
            }

            for (usize level = bloom_extents.size(); level-- > 1;) {
                const RHI::TextureSubresourceRange source_subresources{
                    .base_mip_level = static_cast<u32>(level), .mip_level_count = 1,
                };
                const RHI::TextureSubresourceRange destination_subresources{
                    .base_mip_level = static_cast<u32>(level - 1), .mip_level_count = 1,
                };
                const Core::Extent2D source_extent = bloom_extents[level];
                const Core::Extent2D destination_extent = bloom_extents[level - 1];
                const RHI::TextureViewHandle source_view = slot.bloom_targets.views[level];
                const RHI::BindGroupHandle bind_group = slot.bloom_targets.upsample_bind_groups[level];
                graph.add_render_pass("bloom upsample")
                    .add_color_attachment(RenderGraphColorAttachmentDesc{
                        .texture = bloom_chain,
                        .view = slot.bloom_targets.views[level - 1],
                        .subresources = destination_subresources,
                        .load_op = RHI::LoadOp::Load,
                        .store_op = RHI::StoreOp::Store,
                    })
                    .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                        .texture = bloom_chain, .subresources = source_subresources,
                    })
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.width, .height = destination_extent.height})
                    .set_execute([this, &submission, source_view, source_extent, destination_extent, bind_group](RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(destination_extent.width), .height = static_cast<f32>(destination_extent.height), .min_depth = 0.0f, .max_depth = 1.0f});
                        pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.width, .height = destination_extent.height});
                        return record_bloom_upsample(pass, source_view,
                            glm::vec2{1.0f / static_cast<f32>(source_extent.width), 1.0f / static_cast<f32>(source_extent.height)},
                            submission.render_graph, bind_group);
                    });
            }

            // Explicit HDR composite: the BeforeBloom result plus resolved bloom mip 0 become one
            // scene-linear HDR image, so AfterBloomBeforeToneMap effects (and tonemapping) see a single
            // plain texture and bloom is never sampled a second time later. Imported from the
            // persistent per-frame-slot allocation ensure_frame_composite_target() above just ensured,
            // not graph.create_texture()'d, so this doesn't mint a fresh VkImage/VkImageView every
            // single frame for what is otherwise the exact same resource frame after frame — same
            // Undefined-initial-layout convention every other persistent frame-slot target here uses
            // (DontCare/Clear load ops mean nothing ever needs last frame's contents preserved).
            const RenderGraphTextureHandle composite_destination = graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = slot.composite_target.texture,
                .default_view = slot.composite_target.view,
                .format = submission.deferred_formats.scene_color,
                .extent = frame_extent,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "bloom composite target",
            });
            const RHI::TextureSubresourceRange bloom_mip0{.base_mip_level = 0, .mip_level_count = 1};
            graph.add_render_pass("bloom composite")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = composite_destination,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = post_process_source})
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = bloom_chain, .subresources = bloom_mip0})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
                .set_execute([this, &submission, post_process_source, bloom_chain, render_extent](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(render_extent.width), .height = static_cast<f32>(render_extent.height), .min_depth = 0.0f, .max_depth = 1.0f});
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                    return record_bloom_composite(pass, context.texture(post_process_source).default_view,
                                                  context.texture(bloom_chain).default_view,
                                                  submission.deferred_formats.scene_color,
                                                  submission.render_graph.bloom_intensity,
                                                  submission.transient_bind_groups);
                });
            after_bloom_source = composite_destination;
        }

        after_bloom_source = apply_custom_post_process_stage(after_bloom_source, PostProcessStage::AfterBloomBeforeToneMap);

        // Tonemap post-process: sample the final scene-linear HDR result (bloom already composited in
        // above, if active) and resolve it to the swapchain. recreate_rhi_swapchain() above already picks
        // RGB10A2Unorm/Hdr10St2084 for the swapchain itself once presentation.hdr_enabled is set — the
        // tonemap pipeline's own color-attachment format must match, and the shader must know to
        // PQ-encode instead of relying on an *Srgb format's automatic sRGB OETF (Vulkan applies no
        // equivalent fixed-function curve for PQ).
        const bool hdr_output = static_cast<bool>(record.presentation.hdr_enabled);
        const RHI::Format swapchain_format = hdr_output ? RHI::Format::RGB10A2Unorm : RHI::Format::BGRA8UnormSrgb;
        submission.render_graph.tone_mapping_hdr_output = hdr_output;
        graph.add_render_pass(submission.render_graph.tone_mapping ? "tonemap" : "present scene color")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = swapchain_texture,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = after_bloom_source,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.width, .height = presentation_extent.height})
            .set_execute([this, &submission, presentation_extent, after_bloom_source, swapchain_format](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(presentation_extent.width),
                    .height = static_cast<f32>(presentation_extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.width, .height = presentation_extent.height});
                const RenderGraphTextureAccess source = context.texture(after_bloom_source);
                return record_tonemap(pass, source.default_view, swapchain_format,
                                      submission.render_graph, submission.transient_bind_groups);
            });

        if (submission.render_graph.debug_overlay) {
            // Shaping/residency/instance upload happened above; this pass only issues the prepared
            // instanced draws over the tonemapped scene.
            graph.add_render_pass("debug text overlay")
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = swapchain_texture,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.width, .height = presentation_extent.height})
                .set_execute([this, presentation_extent, &text_overlay_batches](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.width),
                        .height = static_cast<f32>(presentation_extent.height),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.width, .height = presentation_extent.height});
                    const glm::vec2 viewport_size{static_cast<f32>(presentation_extent.width), static_cast<f32>(presentation_extent.height)};
                    return draw_text_overlay(pass, text_overlay_batches, viewport_size);
                });
        }

        if (submission.render_graph.debug_overlay) {
            const f64 seconds = duration<f64>(steady_clock::now() - declare_graph_start).count();
            current_frame_cpu_stage_timings_ms.emplace_back("declare render graph", seconds * 1000.0);
        }
        const bool gpu_timing_enabled = submission.render_graph.debug_overlay;
        if (gpu_timing_enabled) {
            // compile() is pure-CPU and cheap (see its own doc comment) — calling it here just to
            // learn the pass count for query-set sizing, then letting execute() below recompile
            // internally, is simpler than threading a precomputed CompiledPlan through execute()'s
            // public signature for what's a debug-only feature.
            const RenderGraph::CompileResult precompiled = graph.compile();
            const u32 pass_count = precompiled.has_value() ? static_cast<u32>(precompiled->order.size()) : 0;
            if (Core::RendererResult timing_target = ensure_frame_gpu_timing_target(slot, pass_count);
                !timing_target.has_value()) {
                return timing_target;
            }
        }
        {
            ScopedRendererStageTimer timer{"execute render graph", &current_frame_cpu_stage_timings_ms};
            // CPU per-pass timing rides the same debug_overlay gate as GPU per-pass timing
            // (gpu_timing_enabled) even though it needs no query set of its own — keeps the two
            // breakdowns' pass lists in lockstep and avoids per-frame vector churn when the overlay
            // (the only current consumer) is off.
            Core::RendererResult graph_result = gpu_timing_enabled
                ? graph.execute(*device, **encoder, slot.gpu_timing.query_set, &slot.gpu_timing.pending,
                                &slot.cpu_timing.pass_timings)
                : graph.execute(*device, **encoder);
            if (!graph_result.has_value()) {
                return graph_result;
            }
            if (gpu_timing_enabled) {
                slot.gpu_timing.has_pending_results = true;
                slot.cpu_timing.has_pending_results = true;
            }
        }

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            graph.destroy_transient_resources(*device);
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish RHI command encoder"));
        }

        const array command_buffers{*command_buffer};
        const array presented_textures{texture};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .waits = {},
            .signals = {},
            .presented_textures = span<const RHI::SurfaceTexture>{presented_textures.data(), presented_textures.size()},
            .fence = slot.fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer frame submit",
        };
        {
            ScopedRendererStageTimer timer{"submit RHI frame", &current_frame_cpu_stage_timings_ms};
            if (auto submitted = device->submit(submit_desc); !submitted) {
                graph.destroy_transient_resources(*device);
                device->destroy_command_buffer(*command_buffer);
                return unexpected(graphics_error_from_rhi(submitted.error(), "submit RHI frame"));
            }
        }

        // The frame is now in flight. Hand its GPU resources to the ring slot for fence-gated cleanup —
        // deliberately NO wait here (the whole point of the async model). They are reclaimed the next time
        // this slot comes round, after its fence has signaled.
        slot.command_buffer = *command_buffer;
        slot.transient_textures = std::move(submission.retired_text_atlas_resources.textures);
        slot.transient_texture_views = std::move(submission.retired_text_atlas_resources.texture_views);
        graph.take_transient_resources(slot.transient_textures, slot.transient_texture_views);
        slot.transient_bind_groups = std::move(submission.transient_bind_groups);
        slot.transient_buffers = std::move(submission.transient_buffers);
        slot.submitted = true;

        auto presented = [&]() {
            ScopedRendererStageTimer timer{"present RHI frame", &current_frame_cpu_stage_timings_ms};
            return device->present(RHI::PresentDesc{.texture = texture, .label = "renderer present"});
        }();
        if (!presented) {
            if (presented.error().code == RHI::RhiErrorCode::SurfaceLost) {
                record.rhi_swapchain_dirty = true;
            }
            return unexpected(graphics_error_from_rhi(presented.error(), "present RHI frame"));
        }
        if (*presented) {
            record.rhi_swapchain_dirty = true;
        }

        if (submission.render_graph.wait_for_completion) {
            ScopedRendererStageTimer timer{"wait explicitly requested frame completion", &current_frame_cpu_stage_timings_ms};
            if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true); !waited) {
                return unexpected(graphics_error_from_rhi(waited.error(), "wait explicitly requested frame completion"));
            }
        }

        // Stash this call's CPU stage timings on the slot for next-frame readback (see
        // FrameCpuTimingTarget's doc comment) — folding in whatever render_frame/render_frame_dispatch
        // staged before this function ever started (extraction + sort), so the eventual overlay report
        // covers the full CPU frame, not just the RHI-facing tail of it. Only bother when the pass
        // timings above were actually collected (gpu_timing_enabled); otherwise leave the slot's
        // previous (already-consumed) contents alone rather than overwriting them with a partial,
        // untimed-pass picture.
        if (gpu_timing_enabled) {
            slot.cpu_timing.stage_timings = std::move(current_frame_cpu_stage_timings_ms);
            slot.cpu_timing.stage_timings.insert(slot.cpu_timing.stage_timings.end(),
                                                 submission.pre_dispatch_stage_timings_ms.begin(),
                                                 submission.pre_dispatch_stage_timings_ms.end());
        }
        return {};
    }

    Core::RendererResult Renderer::ensure_frame_deferred_targets(FrameInFlight &slot,
                                                                  Core::Extent2D extent,
                                                                  const DeferredTargetFormats &formats) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        const bool matches = slot.deferred_targets.gbuffer_albedo &&
            slot.deferred_targets.depth &&
            slot.deferred_targets.extent.width == extent.width &&
            slot.deferred_targets.extent.height == extent.height &&
            slot.deferred_targets.formats.albedo == formats.albedo &&
            slot.deferred_targets.formats.normal == formats.normal &&
            slot.deferred_targets.formats.material == formats.material &&
            slot.deferred_targets.formats.scene_color == formats.scene_color &&
            slot.deferred_targets.formats.depth == formats.depth;
        if (matches) {
            return {};
        }

        // Bloom's cached first-level descriptor references scene_color_view.
        destroy_frame_bloom_targets(slot);
        destroy_frame_deferred_targets(slot);

        auto create_target = [&](RHI::Format format, RHI::TextureUsage usage,
                                 const char *label) -> Core::RendererExpected<std::pair<RHI::TextureHandle, RHI::TextureViewHandle>> {
            auto texture = device->create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = format,
                .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = usage,
                .label = label,
            });
            if (!texture) {
                return unexpected(graphics_error_from_rhi(texture.error(), label));
            }
            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .label = label,
            });
            if (!view) {
                device->destroy_texture(*texture);
                return unexpected(graphics_error_from_rhi(view.error(), label));
            }
            return std::pair<RHI::TextureHandle, RHI::TextureViewHandle>{*texture, *view};
        };

        constexpr RHI::TextureUsage color_usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled;
        auto albedo = create_target(formats.albedo, color_usage, "persistent deferred gbuffer albedo");
        if (!albedo) return unexpected(albedo.error());
        auto normal = create_target(formats.normal, color_usage, "persistent deferred gbuffer normal");
        if (!normal) {
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(normal.error());
        }
        auto material = create_target(formats.material, color_usage, "persistent deferred gbuffer material");
        if (!material) {
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(material.error());
        }
        auto scene_color = create_target(formats.scene_color, color_usage, "persistent scene color");
        if (!scene_color) {
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(scene_color.error());
        }
        auto depth = create_target(formats.depth,
                                   RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
                                   "persistent deferred depth");
        if (!depth) {
            device->destroy_texture_view(scene_color->second);
            device->destroy_texture(scene_color->first);
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(depth.error());
        }

        slot.deferred_targets = FrameDeferredTargets{
            .extent = extent,
            .formats = formats,
            .gbuffer_albedo = albedo->first,
            .gbuffer_albedo_view = albedo->second,
            .gbuffer_normal = normal->first,
            .gbuffer_normal_view = normal->second,
            .gbuffer_material = material->first,
            .gbuffer_material_view = material->second,
            .scene_color = scene_color->first,
            .scene_color_view = scene_color->second,
            .depth = depth->first,
            .depth_view = depth->second,
        };
        return {};
    }

    void Renderer::destroy_frame_deferred_targets(FrameInFlight &slot) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            auto destroy_target = [device](RHI::TextureHandle texture, RHI::TextureViewHandle view) noexcept {
                if (view) {
                    device->destroy_texture_view(view);
                }
                if (texture) {
                    device->destroy_texture(texture);
                }
            };
            destroy_target(slot.deferred_targets.gbuffer_albedo, slot.deferred_targets.gbuffer_albedo_view);
            destroy_target(slot.deferred_targets.gbuffer_normal, slot.deferred_targets.gbuffer_normal_view);
            destroy_target(slot.deferred_targets.gbuffer_material, slot.deferred_targets.gbuffer_material_view);
            destroy_target(slot.deferred_targets.scene_color, slot.deferred_targets.scene_color_view);
            destroy_target(slot.deferred_targets.depth, slot.deferred_targets.depth_view);
        }
        slot.deferred_targets = {};
    }

    Core::RendererResult Renderer::ensure_frame_bloom_targets(FrameInFlight &slot,
                                                               Core::Extent2D extent,
                                                               u32 requested_levels) {
        requested_levels = std::clamp(requested_levels, 1u, 10u);
        const bool matches = slot.bloom_targets.source_extent.width == extent.width &&
            slot.bloom_targets.source_extent.height == extent.height &&
            slot.bloom_targets.requested_levels == requested_levels &&
            slot.bloom_targets.scene_source_view == slot.deferred_targets.scene_color_view &&
            slot.bloom_targets.texture && !slot.bloom_targets.views.empty() &&
            slot.bloom_targets.downsample_bind_groups.size() == slot.bloom_targets.views.size() &&
            slot.bloom_targets.upsample_bind_groups.size() == slot.bloom_targets.views.size();
        if (matches) return {};

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        destroy_frame_bloom_targets(slot);
        slot.bloom_targets.source_extent = extent;
        slot.bloom_targets.requested_levels = requested_levels;
        slot.bloom_targets.scene_source_view = slot.deferred_targets.scene_color_view;

        Core::Extent2D level_extent{
            .width = std::max(1u, extent.width / 2u),
            .height = std::max(1u, extent.height / 2u),
        };
        for (u32 level = 0; level < requested_levels; ++level) {
            slot.bloom_targets.extents.push_back(level_extent);
            if (level_extent.width == 1u && level_extent.height == 1u) break;
            level_extent.width = std::max(1u, level_extent.width / 2u);
            level_extent.height = std::max(1u, level_extent.height / 2u);
        }

        const Core::Extent2D base_extent = slot.bloom_targets.extents.front();
        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::RG11B10Float,
            .extent = RHI::Extent3D{.width = base_extent.width, .height = base_extent.height, .depth_or_layers = 1},
            .mip_levels = static_cast<u32>(slot.bloom_targets.extents.size()),
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
            .label = "persistent bloom mip chain",
        });
        if (!texture) {
            destroy_frame_bloom_targets(slot);
            return unexpected(graphics_error_from_rhi(texture.error(), "create persistent bloom mip chain"));
        }
        slot.bloom_targets.texture = *texture;
        for (u32 level = 0; level < slot.bloom_targets.extents.size(); ++level) {
            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = level,
                .mip_level_count = 1,
                .label = "persistent bloom mip view",
            });
            if (!view) {
                destroy_frame_bloom_targets(slot);
                return unexpected(graphics_error_from_rhi(view.error(), "create persistent bloom mip view"));
            }
            slot.bloom_targets.views.push_back(*view);
        }

        auto bloom_guard = bloom_.lock();
        if (!bloom_guard->ready || !bloom_guard->sampled_layout) {
            destroy_frame_bloom_targets(slot);
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Bloom pipeline resources are not ready for persistent target binding.");
        }
        auto create_group = [&](RHI::TextureViewHandle source_view) -> Core::RendererExpected<RHI::BindGroupHandle> {
            const array<RHI::BindGroupEntry, 2> entries{
                RHI::BindGroupEntry{.binding = bloom_guard->image_binding, .texture_view = source_view},
                RHI::BindGroupEntry{.binding = bloom_guard->sampler_binding, .sampler = bloom_guard->sampler},
            };
            auto group = device->create_bind_group(RHI::BindGroupDesc{
                .layout = bloom_guard->sampled_layout,
                .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                .label = "persistent bloom source bind group",
            });
            if (!group) return unexpected(graphics_error_from_rhi(group.error(), "create persistent bloom bind group"));
            return *group;
        };

        slot.bloom_targets.downsample_bind_groups.reserve(slot.bloom_targets.views.size());
        slot.bloom_targets.upsample_bind_groups.resize(slot.bloom_targets.views.size());
        for (usize level = 0; level < slot.bloom_targets.views.size(); ++level) {
            const RHI::TextureViewHandle source_view = level == 0
                ? slot.deferred_targets.scene_color_view
                : slot.bloom_targets.views[level - 1];
            auto group = create_group(source_view);
            if (!group) { destroy_frame_bloom_targets(slot); return unexpected(group.error()); }
            slot.bloom_targets.downsample_bind_groups.push_back(*group);
        }
        for (usize level = 1; level < slot.bloom_targets.views.size(); ++level) {
            auto group = create_group(slot.bloom_targets.views[level]);
            if (!group) { destroy_frame_bloom_targets(slot); return unexpected(group.error()); }
            slot.bloom_targets.upsample_bind_groups[level] = *group;
        }
        return {};
    }

    void Renderer::destroy_frame_bloom_targets(FrameInFlight &slot) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            for (RHI::BindGroupHandle group : slot.bloom_targets.downsample_bind_groups) {
                if (group) device->destroy_bind_group(group);
            }
            for (RHI::BindGroupHandle group : slot.bloom_targets.upsample_bind_groups) {
                if (group) device->destroy_bind_group(group);
            }
            for (RHI::TextureViewHandle view : slot.bloom_targets.views) {
                if (view) device->destroy_texture_view(view);
            }
            if (slot.bloom_targets.texture) {
                device->destroy_texture(slot.bloom_targets.texture);
            }
        }
        slot.bloom_targets = {};
    }

    Core::RendererResult Renderer::ensure_frame_composite_target(FrameInFlight &slot,
                                                                  Core::Extent2D extent,
                                                                  RHI::Format format) {
        const bool matches = slot.composite_target.extent.width == extent.width &&
            slot.composite_target.extent.height == extent.height &&
            slot.composite_target.format == format &&
            slot.composite_target.texture && slot.composite_target.view;
        if (matches) return {};

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        destroy_frame_composite_target(slot);
        slot.composite_target.extent = extent;
        slot.composite_target.format = format;

        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = format,
            .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
            .label = "persistent bloom composite target",
        });
        if (!texture) {
            destroy_frame_composite_target(slot);
            return unexpected(graphics_error_from_rhi(texture.error(), "create persistent bloom composite target"));
        }
        slot.composite_target.texture = *texture;

        auto view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = "persistent bloom composite target view",
        });
        if (!view) {
            destroy_frame_composite_target(slot);
            return unexpected(graphics_error_from_rhi(view.error(), "create persistent bloom composite target view"));
        }
        slot.composite_target.view = *view;
        return {};
    }

    void Renderer::destroy_frame_composite_target(FrameInFlight &slot) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            if (slot.composite_target.view) {
                device->destroy_texture_view(slot.composite_target.view);
            }
            if (slot.composite_target.texture) {
                device->destroy_texture(slot.composite_target.texture);
            }
        }
        slot.composite_target = {};
    }

    Core::RendererResult Renderer::ensure_frame_gpu_timing_target(FrameInFlight &slot, u32 required_pass_count) {
        const u32 required_capacity = required_pass_count * 2;
        if (required_capacity == 0 || slot.gpu_timing.capacity >= required_capacity) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        // Growing loses any not-yet-read-back results from this slot's previous query set — harmless,
        // it's a debug-only readout and the very next frame's readback attempt just finds nothing
        // pending (has_pending_results already gets cleared below).
        destroy_frame_gpu_timing_target(slot);

        // A little headroom over the exact requirement so a modest pass-count wobble from optional
        // post-process stages doesn't immediately force another resize.
        const u32 capacity = required_capacity + 16;
        auto query_set = device->create_query_set(RHI::QuerySetDesc{
            .type = RHI::QueryType::Timestamp,
            .count = capacity,
            .label = "renderer gpu pass timing",
        });
        if (!query_set) {
            return unexpected(graphics_error_from_rhi(query_set.error(), "create GPU pass timing query set"));
        }
        slot.gpu_timing.query_set = *query_set;
        slot.gpu_timing.capacity = capacity;
        return {};
    }

    void Renderer::destroy_frame_gpu_timing_target(FrameInFlight &slot) noexcept {
        if (RHI::RhiDevice *device = rhi_device(); device != nullptr && slot.gpu_timing.query_set) {
            device->destroy_query_set(slot.gpu_timing.query_set);
        }
        slot.gpu_timing = {};
    }

    void Renderer::reclaim_frame_slot(FrameInFlight &slot, bool destroy_retired_presentation) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (RHI::BindGroupHandle group : slot.transient_bind_groups) {
                if (group) {
                    device->destroy_bind_group(group);
                }
            }
            for (RHI::BufferHandle buffer : slot.transient_buffers) {
                if (buffer) {
                    device->destroy_buffer(buffer);
                }
            }
            // Views before the textures they alias.
            for (RHI::TextureViewHandle view : slot.transient_texture_views) {
                if (view) {
                    device->destroy_texture_view(view);
                }
            }
            for (RHI::TextureHandle texture : slot.transient_textures) {
                if (texture) {
                    device->destroy_texture(texture);
                }
            }
            // NOT covered by the same guarantee as the transient resources above: a retired
            // swapchain/presentation texture was used by a vkQueuePresentKHR, which isn't fenced the
            // way a command buffer submission is, so this slot's own frame fence signaling doesn't
            // prove the present has finished (VUID-vkDestroySwapchainKHR-swapchain-01282 will fire if
            // you destroy on that assumption — learned the hard way). Only call this with `true` right
            // after a real device->wait_idle() (drain_frames_in_flight / teardown / the periodic
            // maybe_flush_retired_swapchains() bounded flush) — never off a single fence wait.
            if (destroy_retired_presentation) {
                for (RHI::TextureViewHandle view : slot.retired_presentation_texture_views) {
                    if (view) {
                        device->destroy_texture_view(view);
                    }
                }
                for (RHI::TextureHandle texture : slot.retired_presentation_textures) {
                    if (texture) {
                        device->destroy_texture(texture);
                    }
                }
                for (RHI::SwapchainHandle swapchain : slot.retired_swapchains) {
                    if (swapchain) {
                        device->destroy_swapchain(swapchain);
                    }
                }
            }
            if (slot.command_buffer) {
                device->destroy_command_buffer(slot.command_buffer);
            }
        }
        slot.transient_bind_groups.clear();
        slot.transient_buffers.clear();
        slot.transient_texture_views.clear();
        slot.transient_textures.clear();
        if (destroy_retired_presentation) {
            slot.retired_presentation_texture_views.clear();
            slot.retired_presentation_textures.clear();
            slot.retired_swapchains.clear();
        }
        slot.command_buffer = {};
    }

    void Renderer::drain_frames_in_flight(WindowSurfaceRecord &record) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        // Sanctioned heavy wait (teardown / periodic retired-swapchain flush), never the per-frame path.
        device->wait_idle();
        for (FrameInFlight &slot : record.frames_in_flight) {
            // Reclaim unconditionally (not just `if (slot.submitted)`): a slot can carry retired
            // swapchains/presentation textures (attached by recreate_rhi_swapchain onto whichever ring
            // index the *previous* frame used) even on a rare cycle where this exact slot itself was
            // never submitted — e.g. very early in a window's life, before the ring has gone around
            // once. Reclaiming an otherwise-empty slot is a no-op, so this costs nothing normally.
            reclaim_frame_slot(slot, true);
            slot.submitted = false;
            // Leave the fence allocated but unsignaled so the slot is immediately reusable — wait_idle
            // above left every submitted fence signaled, and vkQueueSubmit needs an unsignaled one.
            if (slot.fence) {
                if (auto reset = device->reset_fences(span<const RHI::FenceHandle>{&slot.fence, 1}); !reset) {
                    Foundation::log_warn("Failed to reset drained frame fence: {}", reset.error().message);
                }
            }
        }
    }

    void Renderer::maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept {
        usize retired_count = 0;
        for (const FrameInFlight &slot : record.frames_in_flight) {
            retired_count += slot.retired_swapchains.size();
        }
        // `opportunistic` (called on a frame that isn't itself resizing) flushes any backlog at all —
        // there's no live-resize responsiveness to protect on this frame, so there's no reason to let
        // even one superseded swapchain linger. The non-opportunistic call (from inside an active
        // resize) only trips the bounded safety-net threshold, so a fast continuous drag doesn't pay
        // a wait_idle() on every single frame.
        const usize threshold = opportunistic ? 1 : retired_swapchain_flush_threshold;
        if (retired_count < threshold) {
            return;
        }
        ScopedRendererStageTimer timer{"flush retired swapchains"};
        drain_frames_in_flight(record);
    }

    void Renderer::destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept {
        if (RHI::RhiDevice *device = rhi_device()) {
            // Per-window teardown is allowed to stall. The window is about to disappear, so first make
            // every submitted frame for this surface complete and reclaim frame-owned command buffers,
            // transient targets, and retired swapchains. Without this, Wayland WSI objects backing
            // presented swapchain images can still be attached when SDL destroys the wl_surface, producing
            // "mesa vk display queue ... destroyed while proxies still attached" warnings.
            drain_frames_in_flight(record);
            for (FrameInFlight &slot : record.frames_in_flight) {
                reclaim_frame_slot(slot, true);
                destroy_text_frame_resources(*device, slot.text_overlay_resources);
                destroy_frame_bloom_targets(slot);
                destroy_frame_composite_target(slot);
                destroy_frame_shadow_targets(slot);
                destroy_frame_gpu_timing_target(slot);
                destroy_frame_deferred_targets(slot);
                if (slot.fence) {
                    device->destroy_fence(slot.fence);
                    slot.fence = {};
                }
                slot.submitted = false;
            }
            record.frames_in_flight.clear();
            if (record.depth_view) {
                device->destroy_texture_view(record.depth_view);
            }
            if (record.depth_texture) {
                device->destroy_texture(record.depth_texture);
            }
            if (record.rhi_swapchain) {
                device->destroy_swapchain(record.rhi_swapchain);
            }
            if (record.rhi_surface) {
                device->destroy_surface(record.rhi_surface);
            }
        }
        record.depth_view = {};
        record.depth_texture = {};
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
