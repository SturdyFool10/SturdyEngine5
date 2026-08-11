#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
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

#include <tracy/Tracy.hpp>

using std::array;
using std::chrono::duration;
using std::chrono::steady_clock;
using std::optional;
using std::pair;
using std::span;
using std::string;
using std::unexpected;
using std::unordered_map;
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

        // 2 begin/end timestamp pairs: pre-graph TLAS build, pre-graph photon hash-buffer clear.
        // See FrameInFlight::pregraph_gpu_timing_query_set's doc comment for why these are recorded
        // onto a dedicated fixed-size query set instead of riding RenderGraph::execute_parallel's own.
        constexpr u32 kPregraphGpuTimingQueryCount = 4u;

        class ScopedRendererStageTimer {
          public:
            explicit ScopedRendererStageTimer(const char *stage) noexcept
                : stage_(stage), start_(steady_clock::now()) {}

            // `accumulate_into`: when non-null, this stage's duration is also appended (in
            // milliseconds) so a caller can build a full per-frame CPU stage breakdown — the
            // hitch-warning behavior above is unconditional either way, this is purely additive.
            ScopedRendererStageTimer(const char *stage, vector<pair<string, f64>> *accumulate_into) noexcept
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
            vector<pair<string, f64>> *accumulate_into_ = nullptr;
        };

        // Collapses a pass label's numbered-instance suffix (for example, a bloom mip level) down
        // to its category by truncating at the first digit, so the GPU/CPU timing breakdowns sum
        // same-kind passes into one line. Labels with no digit pass through unchanged.
        [[nodiscard]] UString render_graph_pass_timing_category(const ustr &label) {
            UString category{label};
            const usize digit = category.find_first_of(UString{"0123456789"_ustr});
            if (digit != UString::npos) {
                category.erase(digit);
            }
            while (!category.empty() && category.back() == U' ') {
                category.pop_back();
            }
            return category;
        }

        [[nodiscard]] vector<pair<string, f64>> snapshot_timings(const vector<pair<UString, f64>> &timings) {
            vector<pair<string, f64>> snapshot;
            snapshot.reserve(timings.size());
            for (const auto &[label, milliseconds] : timings) {
                snapshot.emplace_back(label.cpp_string(), milliseconds);
            }
            return snapshot;
        }

        [[nodiscard]] Core::Extent2D framebuffer_extent(Platform::Windowing::Window &window) {
            if (auto size = window.framebuffer_size()) {
                return *size;
            }
            return {};
        }

        // The presentation format/color-space pair for `presentation` — shared by the real swapchain
        // (recreate_rhi_swapchain below) and the offscreen presentation-target texture format used to
        // pick the tonemap pipeline (render_frame_rhi), so both always agree on what "HDR output"
        // means for a given window. RGB10A2Unorm pairs with Hdr10St2084 (10-bit non-linear PQ target);
        // RGBA16Float pairs with ScrgbLinear (float target, no curve — see fullscreen_tonemap.slang).
        [[nodiscard]] RHI::Format hdr_presentation_format(const Core::PresentationSettings &presentation) noexcept {
            if (!static_cast<bool>(presentation.hdr_enabled)) {
                return RHI::Format::BGRA8UnormSrgb;
            }
            switch (presentation.hdr_color_space) {
                // scRGB is linear-with-headroom — needs a float format, not a normalized one.
                case Core::HdrColorSpaceMode::ScrgbLinear: return RHI::Format::RGBA16Float;
                // PQ, HLG, and (best-effort) Dolby Vision all encode to a normalized [0,1] code
                // value in the shader (pqEncode/hlgEncode/pqEncode-as-fallback) — a 10-bit UNORM
                // target is the conventional pairing for all three.
                case Core::HdrColorSpaceMode::Hdr10St2084:
                case Core::HdrColorSpaceMode::Hdr10Hlg:
                case Core::HdrColorSpaceMode::DolbyVision:
                default: return RHI::Format::RGB10A2Unorm;
            }
        }

        [[nodiscard]] RHI::ColorSpace hdr_presentation_color_space(const Core::PresentationSettings &presentation) noexcept {
            if (!static_cast<bool>(presentation.hdr_enabled)) {
                return RHI::ColorSpace::SrgbNonlinear;
            }
            switch (presentation.hdr_color_space) {
                case Core::HdrColorSpaceMode::ScrgbLinear: return RHI::ColorSpace::ScrgbLinear;
                case Core::HdrColorSpaceMode::Hdr10Hlg: return RHI::ColorSpace::Hdr10Hlg;
                case Core::HdrColorSpaceMode::DolbyVision: return RHI::ColorSpace::DolbyVision;
                case Core::HdrColorSpaceMode::Hdr10St2084:
                default: return RHI::ColorSpace::Hdr10St2084;
            }
        }

        [[nodiscard]] const char *hdr_color_space_name(Core::HdrColorSpaceMode mode) noexcept {
            switch (mode) {
                case Core::HdrColorSpaceMode::Hdr10St2084: return "HDR10 (ST2084/PQ)";
                case Core::HdrColorSpaceMode::ScrgbLinear: return "scRGB linear";
                case Core::HdrColorSpaceMode::Hdr10Hlg: return "HLG";
                case Core::HdrColorSpaceMode::DolbyVision: return "Dolby Vision (best-effort)";
            }
            return "unknown";
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
        ZoneScopedN("Renderer::~Renderer");
        wait_idle();
        destroy_all_resources();
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::initialize(
        const Core::RendererCreateInfo &create_info) {
        ZoneScopedN("Renderer::initialize");
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


    Core::RendererResult Renderer::submit_draw(MeshHandle mesh_handle, MaterialInstanceHandle material_handle) {
        ZoneScopedN("Renderer::submit_draw");
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
        ZoneScopedN("Renderer::render_frame");
        // Dev-time shader hot-reload: pick up any edited `.slang` file and rebuild the affected material
        // templates before recording. Cheap when nothing changed (a directory stat); the reload path
        // itself does the one sanctioned wait_idle (see plans/shader-variants-and-hot-reload.md).
        poll_shader_hot_reload();

        FrameSubmission submission{};
        submission.frame_index = desc.frame.frame_index;
        submission.camera = desc.view.camera;
        submission.lighting = desc.view.lighting;
        submission.deferred_formats = desc.view.deferred_formats;
        submission.render_graph = desc.view.render_graph;
        if (!submission.render_graph.render_scene) {
            submission.render_graph.spectral_path_tracing.mode = SpectralRenderMode::RasterDeferred;
        }
        submission.offscreen_target = desc.offscreen_target;
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
        ZoneScopedN("Renderer::render_frame");
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
        ZoneScopedN("Renderer::render_frame_dispatch");
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
            // Stamped after sorting (this draw's *final* position for the frame) so it matches
            // prepare_scene_gpu_data's object_buffer packing order exactly — see object_index's own
            // doc comment (RendererModule.hpp). This snapshot is the only history lookup on the
            // frame's hot path; worker packing then reads the stamped value without contending on the
            // renderer-wide map.
            auto transform_history = previous_world_transforms_.lock();
            for (usize i = 0; i < submission.draws.size(); ++i) {
                RenderItem &item = submission.draws[i];
                item.object_index = static_cast<u32>(i);
                const auto previous = item.stable_id != 0 ? transform_history->find(item.stable_id)
                                                          : transform_history->end();
                item.previous_world_transform = previous != transform_history->end()
                                                    ? previous->second : item.world_transform;
            }
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


    Core::RendererResult Renderer::ensure_rhi_presentation_resources(WindowSurfaceRecord &record) {
        ZoneScopedN("Renderer::ensure_rhi_presentation_resources");
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
        ZoneScopedN("Renderer::render_item_visible");
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
                                                      f32 shadow_slope_bias,
                                                      RHI::SampleCount samples,
                                                      bool with_object_history,
                                                      RHI::BindGroupHandle object_history_group) {
        ZoneScopedN("Renderer::record_render_item");
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

        const bool use_object_history = with_object_history && !depth_only;
        auto pipeline = depth_only
                            ? depth_only_pipeline_for(*material_template_resource, depth_format, shadow_map,
                                                      shadow_depth_bias, shadow_slope_bias, samples)
                            : (use_object_history
                                   ? history_pipeline_for(*material_template_resource, color_formats, depth_format,
                                                          standard_depth_test, samples)
                                   : material_pipeline_for(*material_template_resource, color_formats, depth_format,
                                                           standard_depth_test, samples));
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

        if (use_object_history) {
            // Set 1 for every with-object-history draw regardless of material — bind once per
            // pass/bundle (RenderItemBindingState::bound_object_history_group), same elision pattern
            // as arena_bound below.
            if (!(binding_state.bound_object_history_group == object_history_group)) {
                pass.set_bind_group(1, object_history_group);
                binding_state.bound_object_history_group = object_history_group;
            }
            const ObjectHistoryDrawConstants draw_constants{.object_index = item.object_index};
            pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                    std::as_bytes(span<const ObjectHistoryDrawConstants>{&draw_constants, 1}));
        } else {
            const SceneDrawConstants draw_constants{
                .view_projection = view_projection,
                .model = item.world_transform,
            };
            pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                    std::as_bytes(span<const SceneDrawConstants>{&draw_constants, 1}));
        }

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
        if (index_arena_.buffer && mesh_resource->index_count > 0) {
            pass.draw_indexed(RHI::DrawIndexedArgs{
                .index_count = mesh_resource->index_count,
                .first_index = mesh_resource->index_offset,
                .base_vertex = static_cast<i32>(mesh_resource->vertex_offset),
            });
        } else {
            pass.draw(RHI::DrawArgs{
                .vertex_count = mesh_resource->vertex_count,
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
                                                               bool use_bundles,
                                                               vector<RHI::RenderBundleHandle> &retired_bundles,
                                                               bool shadow_map,
                                                               f32 shadow_depth_bias,
                                                               f32 shadow_slope_bias,
                                                               RHI::SampleCount samples,
                                                               bool with_object_history,
                                                               RHI::BindGroupHandle object_history_group) {
        ZoneScopedN("Renderer::record_render_items_culled");
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
        // full-frame viewport and retain the parallel bundle path. `use_bundles` is trusted as-is
        // (not re-derived from visible.size()/worker_count here) — see this function's doc comment
        // in RendererModule.hpp for why the caller's pre-declared decision must be the sole source
        // of truth.
        if (shadow_map || !use_bundles) {
            RenderItemBindingState binding_state{};
            for (const RenderItem *item : visible) {
                if (Core::RendererResult recorded = record_render_item(
                        pass, *item, color_formats, depth_format, frame_index, view_projection,
                        depth_only, binding_state, standard_depth_test, shadow_map,
                        shadow_depth_bias, shadow_slope_bias, samples, with_object_history, object_history_group);
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
            unordered_map<u64, bool> warmed;
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
            .samples = samples,
            .view_mask = 0,
            .label = bundle_label,
        };

        const usize chunk_count = std::min<usize>(worker_count, visible.size());
        const usize chunk_size = (visible.size() + chunk_count - 1) / chunk_count;

        struct ChunkResult {
            Core::RendererResult status{};
            RHI::RenderBundleHandle bundle{};
            unique_ptr<RHI::RenderBundleEncoder> encoder;
        };
        vector<ChunkResult> results(chunk_count);

        // Pool/buffer *creation* happens here, serially, on this thread — not inside the spawned
        // tasks below. Same fix as the shadow-atlas parallel path's own identical issue (see that
        // code's comment, and memory project_render_threading): concurrent
        // vkCreateCommandPool/vkAllocateCommandBuffers from multiple worker threads corrupts the heap
        // on this RADV setup even though each thread creates its own independent pool/buffer. Only
        // the actual *recording* is dispatched to worker threads below.
        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
            const usize begin = chunk * chunk_size;
            const usize end = std::min(visible.size(), begin + chunk_size);
            if (begin >= end) {
                continue;
            }
            auto encoder = device->create_render_bundle_encoder(bundle_desc);
            if (!encoder) {
                return unexpected(graphics_error_from_rhi(encoder.error(), "create render bundle encoder"));
            }
            results[chunk].encoder = std::move(*encoder);
        }

        vector<Async::TaskHandle<void>> tasks;
        tasks.reserve(chunk_count);
        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
            const usize begin = chunk * chunk_size;
            const usize end = std::min(visible.size(), begin + chunk_size);
            if (begin >= end) {
                continue;
            }
            tasks.push_back(Async::Scheduler::spawn([this, &visible, &results, chunk, begin, end,
                                                      color_formats, depth_format, frame_index, view_projection,
                                                      depth_only, standard_depth_test, shadow_map,
                                                      shadow_depth_bias, shadow_slope_bias, samples,
                                                      with_object_history, object_history_group]() {
                RHI::RenderBundleEncoder &encoder = *results[chunk].encoder;
                RenderItemBindingState binding_state{};
                for (usize i = begin; i < end; ++i) {
                    if (Core::RendererResult recorded = record_render_item(
                            encoder, *visible[i], color_formats, depth_format, frame_index, view_projection,
                            depth_only, binding_state, standard_depth_test, shadow_map,
                            shadow_depth_bias, shadow_slope_bias, samples, with_object_history, object_history_group);
                        !recorded.has_value()) {
                        results[chunk].status = recorded;
                        return;
                    }
                }
                auto finished = encoder.finish();
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
        // Bundles must outlive this frame's own command buffers — execute_bundles only records a
        // reference for the GPU to run later, it does not run or even submit anything. Destroying
        // them here would be a use-after-free once the GPU actually executes the frame. Defer to
        // FrameInFlight's fence-gated cleanup instead (see FrameSubmission::transient_render_bundles).
        retired_bundles.insert(retired_bundles.end(), bundles.begin(), bundles.end());
        if (has_error) {
            return first_error;
        }
        return {};
    }

    template <typename Encoder>
    Core::RendererResult Renderer::record_shadow_view_chunk(Encoder &encoder,
                                                             span<const ShadowRenderView> views,
                                                             span<const RenderItem> draws,
                                                             RHI::Format depth_format,
                                                             u64 frame_index,
                                                             f32 shadow_depth_bias,
                                                             f32 shadow_slope_bias) {
        ZoneScopedN("Renderer::record_shadow_view_chunk");
        RenderItemBindingState binding_state{};
        for (const ShadowRenderView &view : views) {
            encoder.set_viewport(RHI::Viewport{
                .x = static_cast<f32>(view.viewport.x),
                .y = static_cast<f32>(view.viewport.y),
                .width = static_cast<f32>(view.viewport.width),
                .height = static_cast<f32>(view.viewport.height),
                .min_depth = 0.0f,
                .max_depth = 1.0f,
            });
            encoder.set_scissor(view.viewport);
            for (const RenderItem &item : draws) {
                if (!render_item_visible(item, view.frustum)) {
                    continue;
                }
                if (Core::RendererResult recorded = record_render_item(
                        encoder, item, span<const RHI::Format>{}, depth_format, frame_index,
                        view.view_projection, /*depth_only=*/true, binding_state,
                        /*standard_depth_test=*/false, /*shadow_map=*/true, shadow_depth_bias,
                        shadow_slope_bias, RHI::SampleCount::X1, false, RHI::BindGroupHandle{});
                    !recorded.has_value()) {
                    return recorded;
                }
            }
        }
        return {};
    }

    Core::RendererResult Renderer::ensure_rhi_depth_resources(WindowSurfaceRecord &record) {
        ZoneScopedN("Renderer::ensure_rhi_depth_resources");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (Core::is_zero(record.swapchain_extent)) {
            return {};
        }
        if (record.depth_texture && record.depth_view) {
            return {};
        }

        auto depth_texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = record.depth_format,
            .extent = RHI::Extent3D{.width = record.swapchain_extent.x,
                                    .height = record.swapchain_extent.y,
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

    Core::RendererResult Renderer::drain_pending_present(WindowSurfaceRecord &record,
                                                         vector<pair<string, f64>> *stage_timings_ms) {
        ZoneScopedN("Renderer::drain_pending_present");
        if (!record.pending_present) {
            return {};
        }
        RHI::RhiExpected<RHI::PresentOutcome> presented = [&]() {
            ScopedRendererStageTimer timer{"wait pending present", stage_timings_ms};
            return record.pending_present->wait();
        }();
        record.pending_present.reset();
        const optional<RHI::FenceHandle> completion_fence = record.pending_present_completion_fence;
        record.pending_present_completion_fence.reset();
        if (stage_timings_ms != nullptr) {
            stage_timings_ms->emplace_back("present queue lock wait", record.last_present_lock_wait_ms);
        }
        if (!presented) {
            // A failed vkQueuePresentKHR never signals a maintenance completion fence. Discard it
            // instead of leaving an unsignaled fence attached to a future retired swapchain.
            if (completion_fence) {
                std::erase(record.active_presentation_completion_fences, *completion_fence);
                if (RHI::RhiDevice *device = rhi_device()) {
                    device->destroy_fence(*completion_fence);
                }
            }
            // SurfaceLost: the swapchain must be recreated before presenting again -- the existing
            // dirty-flag path already does that (a true surface-loss recovery, distinct from a mere
            // swapchain rebuild, is Phase 3 scope per the sync/presentation rework plan). Reachable
            // now for the first time: VulkanQueue::present previously folded VK_ERROR_SURFACE_LOST_KHR
            // into a generic OperationFailed, so this branch was dead code.
            //
            // FullScreenExclusiveLost: a normal, recoverable state transition (alt-tab, focus loss),
            // not a device/surface failure -- no exclusive-fullscreen state exists in the engine yet
            // to clear, so this becomes the same dirty-flag response until that support itself exists.
            if (presented.error().code == RHI::RhiErrorCode::SurfaceLost ||
                presented.error().code == RHI::RhiErrorCode::FullScreenExclusiveLost) {
                record.rhi_swapchain_dirty = true;
            }
            return unexpected(graphics_error_from_rhi(presented.error(), "present RHI frame"));
        }
        switch (*presented) {
            case RHI::PresentOutcome::Success:
                break;
            case RHI::PresentOutcome::Suboptimal:
            case RHI::PresentOutcome::OutOfDate:
                record.rhi_swapchain_dirty = true;
                break;
        }
        return {};
    }

    Core::RendererResult Renderer::recreate_rhi_swapchain(WindowSurfaceRecord &record, u64 frame_index,
                                                          optional<Core::Extent2D> known_extent) {
        ZoneScopedN("Renderer::recreate_rhi_swapchain");
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
        if (Core::is_zero(extent)) {
            record.rhi_swapchain_dirty = true;
            return {};
        }

        const RHI::SwapchainHandle old_swapchain = record.rhi_swapchain;
        const RHI::TextureHandle old_depth_texture = record.depth_texture;
        const RHI::TextureViewHandle old_depth_view = record.depth_view;

        RHI::SwapchainDesc swapchain_desc{
            .surface = record.rhi_surface,
            .width = extent.x,
            .height = extent.y,
            .format = hdr_presentation_format(record.presentation),
            .color_space = hdr_presentation_color_space(record.presentation),
            // Core::resolve_present_strategy() is the one place vsync/variable_refresh/latency/
            // preference get interpreted together (Core/Renderer.hpp) — the backend then resolves
            // that strategy against this surface's real supported present modes
            // (RHI::choose_present_mode(), RHI/Swapchain.hpp), never a raw requested PresentMode.
            .present_strategy = Core::resolve_present_strategy(record.presentation),
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::TransferDst,
            .composite_alpha = RHI::CompositeAlphaMode::Auto,
            .clipped = true,
            .image_count = record.presentation.swapchain_image_count != 0
                               ? record.presentation.swapchain_image_count
                               : record.desired_frames_in_flight + 1,
            // Sizes the backend's acquisition-semaphore ring, independent of image_count above — see
            // RHI::SwapchainDesc::frames_in_flight's own doc comment. capabilities_.max_frames_in_flight
            // is the one resolved value every frames-in-flight-derived subsystem consumes (Phase 1 of
            // the sync/presentation rework — Core::resolve_frames_in_flight, Core/Renderer.hpp).
            .frames_in_flight = capabilities_.max_frames_in_flight,
            .old_swapchain = old_swapchain,
            .allow_present_from_compute = static_cast<bool>(record.presentation.allow_present_from_compute),
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
            if (device->is_enabled(RHI::Feature::SwapchainMaintenance)) {
                // A present-completion fence signals after the presentation engine has finished with
                // the image, which is the missing lifetime proof a graphics submission fence cannot
                // provide. Move every still-live fence with this exact swapchain generation.
                record.retired_presentation_resources.push_back(RetiredPresentationResources{
                    .swapchain = old_swapchain,
                    .depth_texture = old_depth_texture,
                    .depth_view = old_depth_view,
                    .completion_fences = std::move(record.active_presentation_completion_fences),
                });
            } else {
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
        }
        return ensure_rhi_depth_resources(record);
    }

    Core::RendererResult Renderer::render_frame_rhi(WindowSurfaceRecord &record,
                                                    const Core::FrameInput &frame,
                                                    FrameSubmission &submission) {
        ZoneScopedN("Renderer::render_frame_rhi");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        if (Core::RendererResult spectral_ready = ensure_spectral_path_tracing_resources(
                submission.render_graph.spectral_path_tracing.mode);
            !spectral_ready.has_value()) {
            return spectral_ready;
        }
        if (submission.render_graph.spectral_path_tracing.mode != SpectralRenderMode::RasterDeferred) {
            if (Core::RendererResult acceleration_ready =
                    ensure_spectral_mesh_acceleration_structures(submission.draws);
                !acceleration_ready.has_value()) {
                return acceleration_ready;
            }
        }

        // N-buffered in-flight ring, keyed by frame_index so it tracks the material system's per-frame
        // UBO slot (frame_index % N). (Re)size on the first frame or after a capability change (device-loss
        // recovery clears the ring). Lives on the window's own record, not a Renderer-wide member, since
        // each window has its own swapchain and therefore its own frame-in-flight lifetime.
        // capabilities_.max_frames_in_flight is already >= 1 by construction — resolved exactly once,
        // through Core::resolve_frames_in_flight, at backend initialization (VulkanBackendDevice.cpp)
        // — so no local zero-guard is needed here.
        const u32 frame_count = capabilities_.max_frames_in_flight;
        if (record.frames_in_flight.size() != frame_count) {
            for (FrameInFlight &old_slot : record.frames_in_flight) {
                destroy_text_frame_resources(*device, old_slot.text_overlay_resources);
                destroy_frame_bloom_targets(old_slot);
                destroy_frame_composite_target(old_slot);
                destroy_frame_gpu_timing_target(old_slot);
                destroy_frame_pregraph_gpu_timing_target(old_slot);
                destroy_frame_shadow_targets(old_slot);
                destroy_frame_atmosphere_targets(old_slot);
                destroy_frame_spectral_photon_targets(old_slot);
                destroy_frame_deferred_targets(old_slot);
            }
            record.frames_in_flight.assign(frame_count, FrameInFlight{});
        }
        const u32 frame_slot_index = static_cast<u32>(frame.frame_index % frame_count);
        FrameInFlight &slot = record.frames_in_flight[frame_slot_index];
        if (!slot.submitted && (!slot.transient_buffers.empty() || !slot.transient_bind_groups.empty() ||
                                !slot.transient_acceleration_structures.empty())) {
            // A prior recording attempt failed before submission. Those resources were never visible to
            // the GPU and can be reclaimed immediately before this slot is reused.
            reclaim_frame_slot(slot, false);
        }

        // Collects this call's own CPU stage costs (wait fence, swapchain recreate/acquire, graph
        // execute, submit, present — whichever of those actually run this frame) so they can be
        // stashed on `slot` for the debug overlay to display next time this ring slot comes round —
        // see FrameCpuTimingTarget's doc comment for why "next time", not "this frame".
        vector<pair<string, f64>> current_frame_cpu_stage_timings_ms;

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
                auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true);
                if (!waited) {
                    return unexpected(graphics_error_from_rhi(waited.error(), "wait in-flight frame fence"));
                }
                if (!*waited) {
                    // A real timeout (this call uses wait_forever, so this should be unreachable
                    // outside a device hang) -- not an error, but resource reclamation below must
                    // not run: the fence is not confirmed signaled.
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "wait in-flight frame fence: vkWaitForFences timed out.");
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
        vector<pair<UString, f64>> gpu_pass_timings_ms;
        if (slot.gpu_timing.has_pending_results) {
            const f32 period_ns = device->limits().timestamp_period_ns;
            unordered_map<UString, f64> totals_ms;
            bool any_read = false;
            // Accumulates one query set's begin/end pairs into `totals_ms` — shared between the
            // RenderGraph's own per-pass query set and the fixed-size pre-graph one (TLAS build,
            // photon hash clear) recorded before the graph exists, so "GPU total" below reflects
            // both instead of silently under-reporting the pre-graph cost.
            const auto accumulate_query_set = [&](RHI::QuerySetHandle query_set,
                                                   const vector<RenderGraph::GpuPassTiming> &timings) {
                if (!query_set || timings.empty()) {
                    return;
                }
                // Only the slots actually reset+written this specific prior frame are valid to
                // read — not the query set's full allocated capacity, which can exceed that
                // (headroom from resize policy, or a larger pass count from an earlier frame that
                // grew capacity but isn't this frame's). Reading an unwritten slot is a real
                // "query not reset" validation error, not just wasted work.
                u32 used_query_count = 0;
                for (const RenderGraph::GpuPassTiming &timing : timings) {
                    used_query_count = std::max(used_query_count, timing.begin_query_index + 1);
                    used_query_count = std::max(used_query_count, timing.end_query_index + 1);
                }
                vector<u64> raw_ticks(used_query_count, 0);
                auto read = device->get_query_set_results(
                    query_set, 0, used_query_count,
                    std::as_writable_bytes(span<u64>{raw_ticks.data(), raw_ticks.size()}), sizeof(u64),
                    RHI::QueryResultFlags::Result64Bit | RHI::QueryResultFlags::Wait);
                if (!read.has_value()) {
                    return;
                }
                any_read = true;
                for (const RenderGraph::GpuPassTiming &timing : timings) {
                    if (timing.begin_query_index >= raw_ticks.size() || timing.end_query_index >= raw_ticks.size()) {
                        continue;
                    }
                    const u64 begin_ticks = raw_ticks[timing.begin_query_index];
                    const u64 end_ticks = raw_ticks[timing.end_query_index];
                    if (end_ticks <= begin_ticks) {
                        continue;
                    }
                    const f64 ms = static_cast<f64>(end_ticks - begin_ticks) * static_cast<f64>(period_ns) / 1.0e6;
                    totals_ms[render_graph_pass_timing_category(timing.label.as_ustr())] += ms;
                }
            };
            if (period_ns > 0.0f) {
                accumulate_query_set(slot.gpu_timing.query_set, slot.gpu_timing.pending);
                accumulate_query_set(slot.pregraph_gpu_timing_query_set, slot.pregraph_gpu_timing_pending);
            }
            if (any_read) {
                gpu_pass_timings_ms.assign(totals_ms.begin(), totals_ms.end());
                std::sort(gpu_pass_timings_ms.begin(), gpu_pass_timings_ms.end(),
                         [](const auto &a, const auto &b) { return a.second > b.second; });
                // Published independent of draw_overlay_text (Scene.hpp) — a caller building its own
                // display via Renderer::last_frame_timings() needs this even when the on-screen text
                // block further down is skipped.
                auto published_timings = record.last_frame_timings->lock();
                published_timings->gpu_pass_timings_ms = snapshot_timings(gpu_pass_timings_ms);
                published_timings->has_data = true;
            }
            slot.gpu_timing.has_pending_results = false;
        }

        // CPU pass/stage timing readback — no query/fence dependency (it's wall-clock CPU time,
        // ready the instant last frame's render_frame_rhi call returned), but still read back here
        // rather than computed fresh below: this frame's debug-overlay text is built (see further
        // down) before this frame's own RenderGraph::execute() call has run, so "this frame's own
        // numbers" don't exist yet either way — same one-frame-stale contract as GPU timing above,
        // just for a different reason.
        vector<pair<UString, f64>> cpu_pass_timings_ms;
        vector<pair<string, f64>> cpu_stage_timings_ms;
        if (slot.cpu_timing.has_pending_results) {
            unordered_map<UString, f64> totals_ms;
            for (const RenderGraph::CpuPassTiming &timing : slot.cpu_timing.pass_timings) {
                totals_ms[render_graph_pass_timing_category(timing.label.as_ustr())] += timing.duration_ms;
            }
            cpu_pass_timings_ms.assign(totals_ms.begin(), totals_ms.end());
            std::sort(cpu_pass_timings_ms.begin(), cpu_pass_timings_ms.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
            cpu_stage_timings_ms = slot.cpu_timing.stage_timings;
            // See the matching GPU-side comment above: published regardless of draw_overlay_text.
            auto published_timings = record.last_frame_timings->lock();
            published_timings->cpu_pass_timings_ms = snapshot_timings(cpu_pass_timings_ms);
            published_timings->cpu_stage_timings_ms = cpu_stage_timings_ms;
            published_timings->has_data = true;
            slot.cpu_timing.has_pending_results = false;
        }

        if (!slot.fence) {
            auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer frame fence"});
            if (!fence) {
                return unexpected(graphics_error_from_rhi(fence.error(), "create RHI frame fence"));
            }
            slot.fence = *fence;
        }

        const bool offscreen_output = static_cast<bool>(submission.offscreen_target);
        optional<ResolvedOffscreenRenderTarget> resolved_offscreen;
        Core::Extent2D presentation_extent{};
        if (offscreen_output) {
            resolved_offscreen = resolve_offscreen_render_target(submission.offscreen_target);
            if (!resolved_offscreen) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Render graph selected an unknown or destroyed off-screen target.");
            }
            presentation_extent = resolved_offscreen->extent;
        } else {
            if (Core::RendererResult resources = ensure_rhi_presentation_resources(record); !resources.has_value()) {
                return resources;
            }

            // Must happen before rhi_swapchain_dirty (read just below) or any recreate/retire-flush of
            // the swapchain — see drain_pending_present's and WindowSurfaceRecord::pending_present's
            // doc comments. In steady state (GPU keeping up) the previous frame's present has already
            // finished by the time we get here, so this is a no-op wait.
            //
            // During a modal live resize, however, waiting here turns an asynchronous present straight
            // back into a render-thread stall. Keep the previous swapchain image available for the
            // platform compositor and let Application retry this coalesced frame at its bounded cadence
            // once the present task completes. Normal frames preserve the strict wait so a resize outside
            // the live path still observes presentation errors before touching swapchain lifetime.
            if (frame.live_resize && record.pending_present && !record.pending_present->is_done()) {
                return {};
            }
            if (Core::RendererResult drained = drain_pending_present(record, &current_frame_cpu_stage_timings_ms);
                !drained.has_value()) {
                return drained;
            }
            reclaim_completed_presentation_fences(record);

            // Framebuffer size comes from FrameInput (already fresh from whichever thread owns the window
            // this tick — see Application::render_managed_window), never by re-querying the Window object.
            const Core::Extent2D surface_extent{frame.framebuffer_width, frame.framebuffer_height};
            if (Core::is_zero(surface_extent)) {
                return {};
            }
            const bool size_changed = surface_extent != record.swapchain_extent;
            const bool should_recreate = record.rhi_swapchain_dirty || size_changed;
            if (should_recreate) {
                {
                    ScopedRendererStageTimer timer{"recreate swapchain", &current_frame_cpu_stage_timings_ms};
                    if (Core::RendererResult recreated = recreate_rhi_swapchain(
                            record, frame.frame_index, surface_extent);
                        !recreated.has_value()) {
                        return recreated;
                    }
                }
                maybe_flush_retired_swapchains(record, false);
            } else {
                maybe_flush_retired_swapchains(record, true);
            }
            if (!record.rhi_swapchain) {
                return {};
            }
            presentation_extent = record.swapchain_extent;
        }

        const bool hdr_output = !offscreen_output && static_cast<bool>(record.presentation.hdr_enabled);
        const RHI::Format output_format = hdr_output ? hdr_presentation_format(record.presentation) : RHI::Format::BGRA8UnormSrgb;
        const f32 resolution_scale = std::clamp(submission.render_graph.resolution_scale, 0.1f, 2.0f);
        const Core::Extent2D render_extent = glm::max(
            Core::Extent2D{std::lround(static_cast<f64>(presentation_extent.x) * resolution_scale),
                           std::lround(static_cast<f64>(presentation_extent.y) * resolution_scale)},
            Core::Extent2D{1u, 1u});
        {
            ScopedRendererStageTimer timer{"prepare scene GPU data", &current_frame_cpu_stage_timings_ms};
            if (Core::RendererResult scene_gpu_data = prepare_scene_gpu_data(record, frame.frame_index, submission); !scene_gpu_data.has_value()) {
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
        // capabilities_.max_frames_in_flight is already >= 1 by construction (see the frame_count
        // comment above) — no local zero-guard needed.
        const u32 scene_frame_count = capabilities_.max_frames_in_flight;
        SceneFrameGpuResources &instance_cull_resources = record.scene_frame_resources[frame.frame_index % scene_frame_count];
        // One bind group for every with-object-history draw this frame (RenderItem::object_index
        // indexes the same object_buffer/view_buffer prepare_scene_gpu_data already populated above,
        // for both instanced and non-instanced draws) — built once here rather than per-draw.
        RHI::BindGroupHandle object_history_group{};
        if (submission.render_graph.render_scene && !submission.draws.empty()) {
            auto history_group = ensure_object_history_bind_group(instance_cull_resources, submission.transient_bind_groups);
            if (!history_group) {
                return unexpected(history_group.error());
            }
            object_history_group = *history_group;
        }
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

        const bool full_path_tracing = submission.render_graph.spectral_path_tracing.mode ==
                                       SpectralRenderMode::FullPathTracing;
        const u32 requested_msaa = full_path_tracing
            ? 1u : std::min(std::max(submission.render_graph.msaa_samples, 1u), 8u);
        const u32 supported_msaa = device->limits().framebuffer_sample_counts;
        const RHI::SampleCount framebuffer_samples =
            requested_msaa >= 8u && (supported_msaa & 8u) != 0 ? RHI::SampleCount::X8 :
            requested_msaa >= 4u && (supported_msaa & 4u) != 0 ? RHI::SampleCount::X4 :
            requested_msaa >= 2u && (supported_msaa & 2u) != 0 ? RHI::SampleCount::X2 :
            RHI::SampleCount::X1;
        if (Core::RendererResult deferred_targets = ensure_frame_deferred_targets(
                slot, render_extent, submission.deferred_formats, framebuffer_samples);
            !deferred_targets.has_value()) {
            return deferred_targets;
        }

        u64 spectral_accumulation_signature = 1469598103934665603ull;
        // Same FNV-1a signature as spectral_accumulation_signature, but deliberately never hashes the
        // view-projection matrix — the caustic photon map is view-independent (depends only on
        // geometry + sun + photon settings), so folding the camera transform in here would force a
        // full photon re-emission (up to 262144 photons, each up to max_bounces ray-traced segments,
        // plus a 2 MiB hash-head buffer clear) on every camera pan/rotate for no reason. Everything
        // that genuinely invalidates the cached map (geometry, sun, photon settings) is still hashed
        // into both signatures identically via hash_u64_both/hash_float_both/hash_matrix_both below.
        u64 spectral_photon_signature = 1469598103934665603ull;
        bool spectral_accumulation_reset = false;
        if (full_path_tracing) {
            if (Core::RendererResult accumulation_target = ensure_spectral_accumulation_target(record, render_extent);
                !accumulation_target.has_value()) {
                return accumulation_target;
            }

            const auto hash_u64 = [&](u64 value) {
                spectral_accumulation_signature ^= value;
                spectral_accumulation_signature *= 1099511628211ull;
            };
            const auto hash_u64_both = [&](u64 value) {
                hash_u64(value);
                spectral_photon_signature ^= value;
                spectral_photon_signature *= 1099511628211ull;
            };
            const auto hash_float = [&](f32 value) { hash_u64(std::bit_cast<u32>(value)); };
            const auto hash_float_both = [&](f32 value) { hash_u64_both(std::bit_cast<u32>(value)); };
            const auto hash_matrix = [&](const glm::mat4 &matrix) {
                for (u32 column = 0; column < 4u; ++column) {
                    for (u32 row = 0; row < 4u; ++row) {
                        hash_float(matrix[column][row]);
                    }
                }
            };
            const auto hash_matrix_both = [&](const glm::mat4 &matrix) {
                for (u32 column = 0; column < 4u; ++column) {
                    for (u32 row = 0; row < 4u; ++row) {
                        hash_float_both(matrix[column][row]);
                    }
                }
            };
            // Accumulation is screen-space and the shader reads/writes one history image in place, so
            // camera motion must invalidate it. The view-independent photon signature intentionally
            // continues to exclude the camera transform.
            hash_matrix(submission.view_projection);
            hash_u64_both(render_extent.x);
            hash_u64_both(render_extent.y);
            hash_float_both(submission.lighting.sun.direction.x);
            hash_float_both(submission.lighting.sun.direction.y);
            hash_float_both(submission.lighting.sun.direction.z);
            hash_float_both(submission.lighting.sun.radiance.x);
            hash_float_both(submission.lighting.sun.radiance.y);
            hash_float_both(submission.lighting.sun.radiance.z);
            hash_float(submission.render_graph.background_intensity);
            const SpectralPathTracingSettings &spectral = submission.render_graph.spectral_path_tracing;
            hash_u64_both(spectral.samples_per_pixel);
            hash_u64_both(spectral.max_bounces);
            hash_u64_both(spectral.russian_roulette_start_bounce);
            hash_u64_both(spectral.photon_count);
            hash_float_both(spectral.caustic_gather_radius);
            hash_float_both(spectral.wavelength_min_nm);
            hash_float_both(spectral.wavelength_max_nm);
            hash_u64_both(submission.draws.size());
            for (const RenderItem &item : submission.draws) {
                hash_u64_both(item.stable_id);
                hash_u64_both(item.mesh.value);
                hash_u64_both(item.material.value);
                if (const MaterialInstanceResource *material = material_instance(item.material)) {
                    hash_u64_both(material->content_revision);
                }
                hash_matrix_both(item.world_transform);
            }
            const SpectralAccumulationTarget &history = record.spectral_accumulation;
            spectral_accumulation_reset = !history.initialized ||
                history.state_signature != spectral_accumulation_signature ||
                history.last_frame_index + 1u != frame.frame_index;
        }
        // Snapshotted here, before ensure_hiz_pyramid can touch has_valid_data (on first build /
        // resize) and well before this frame's own "hiz build mip" passes overwrite the pyramid's
        // contents later on — so this is genuinely *last* completed frame's finished pyramid, for the
        // "gpu instance cull" pass below (which runs before this frame's own depth exists at all —
        // see its own comment) to occlusion-test against. See HiZPyramidTargets's doc comment.
        HiZCullInput hiz_cull_input{};
        if (Core::RendererResult hiz_build_ready = ensure_hiz_build_resources(); !hiz_build_ready.has_value()) {
            return hiz_build_ready;
        }
        {
            if (Core::RendererResult pyramid_ready = ensure_hiz_pyramid(record.hiz_pyramid, render_extent);
                !pyramid_ready.has_value()) {
                return pyramid_ready;
            }
            hiz_cull_input = HiZCullInput{
                .pyramid_view = record.hiz_pyramid.full_view,
                .extent_width = record.hiz_pyramid.extent.x,
                .extent_height = record.hiz_pyramid.extent.y,
                .mip_count = record.hiz_pyramid.mip_levels,
                .valid = record.hiz_pyramid.has_valid_data,
            };
        }
        PreparedShadowFrame shadow_frame{};
        if (submission.render_graph.render_scene) {
            const SpectralIntegratorPolicy integrator_policy = spectral_integrator_policy(
                submission.render_graph.spectral_path_tracing.mode);
            const u32 requested_shadow_atlas = submission.render_graph.shadows &&
                                                       integrator_policy.raster_shadow_atlas
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
                                                                            shadow_frame, render_extent);
                !shadow_prepared.has_value()) {
                return shadow_prepared;
            }
            if (Core::RendererResult atmosphere_targets = ensure_frame_atmosphere_targets(slot);
                !atmosphere_targets.has_value()) {
                return atmosphere_targets;
            }
            if (Core::RendererResult atmosphere_prepared =
                    prepare_atmosphere_frame(submission, slot.atmosphere_targets.constants_buffer);
                !atmosphere_prepared.has_value()) {
                return atmosphere_prepared;
            }
            if (Core::RendererResult atmosphere_resources = ensure_atmosphere_lut_resources();
                !atmosphere_resources.has_value()) {
                return atmosphere_resources;
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
            if (Core::RendererResult bloom_targets = ensure_frame_bloom_targets(
                    slot, render_extent, submission.render_graph.bloom_max_levels,
                    submission.render_graph.bloom_downsample_ratio);
                !bloom_targets.has_value()) {
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
        if (Core::RendererResult aa_ready = ensure_post_process_aa_resources(
                submission.render_graph, submission.deferred_formats.scene_color);
            !aa_ready.has_value()) {
            return aa_ready;
        }
        for (const CustomPostProcessEffect &effect : submission.render_graph.custom_post_processes) {
            if (Core::RendererResult custom_ready = ensure_custom_post_process(effect, submission.deferred_formats.scene_color); !custom_ready.has_value()) {
                return custom_ready;
            }
        }
        for (const CustomGraphPass &pass : submission.render_graph.custom_graph.passes) {
            if (pass.kind == CustomGraphPassKind::RasterEffect) {
                if (Core::RendererResult ready = ensure_custom_post_process(
                        pass.raster, submission.deferred_formats.scene_color);
                    !ready.has_value()) {
                    return ready;
                }
            } else if (pass.kind == CustomGraphPassKind::ComputeEffect) {
                if (Core::RendererResult ready = ensure_custom_compute_effect(pass.compute); !ready.has_value()) {
                    return ready;
                }
            }
        }
        if (Core::RendererResult tonemap_ready = ensure_tonemap_resources(); !tonemap_ready.has_value()) {
            return tonemap_ready;
        }
        if (auto tonemap_pipeline = tonemap_pipeline_for(output_format); !tonemap_pipeline) {
            return unexpected(tonemap_pipeline.error());
        }

        // Ensures a successfully-acquired swapchain image is never abandoned: unless explicitly
        // disarmed once its acquisition semaphore is safely consumed by a successful submit(), any
        // early return below forces a full swapchain rebuild (fresh semaphores) instead of risking
        // the next acquire on this frame slot observing a still-signaled semaphore -- Vulkan
        // requires it unsignaled at acquire time, and there is no explicit release path
        // (VK_EXT_swapchain_maintenance1) wired in here to un-signal it any other way. A single
        // guard covering every exit path, rather than ad hoc handling at each one -- see "successful
        // acquisition must always resolve" (Phase 2 of the sync/presentation rework).
        struct AcquiredImageGuard {
            WindowSurfaceRecord *record = nullptr;
            bool resolved = false;
            ~AcquiredImageGuard() noexcept {
                if (record != nullptr && !resolved) {
                    record->rhi_swapchain_dirty = true;
                }
            }
        } acquired_image_guard;

        // The expensive acquisition-independent TLAS preparation below runs before acquire so an image
        // is not held through that CPU/GPU-recording work. Stateful atlas preparation deliberately does
        // not: TextAtlas publishes new residency/layout metadata as it records uploads, so a normal
        // NotReady acquire must be resolved before those commands are recorded or a skipped frame would
        // leave glyphs marked resident without ever uploading them. Spectral photon preparation follows
        // the same post-acquire rule for its optimistic `populated` bookkeeping.
        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer frame"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create RHI command encoder"));
        }
        // Known from submission alone, so computable this early — used below to time the pre-graph
        // TLAS build/photon hash clear (which happen before the RenderGraph is even declared, let
        // alone compiled) and reused again once execute_parallel's own per-pass timing is wired up.
        const bool gpu_timing_enabled = submission.render_graph.debug_overlay;
        if (gpu_timing_enabled) {
            if (Core::RendererResult pregraph_timing = ensure_frame_pregraph_gpu_timing_target(slot);
                !pregraph_timing.has_value()) {
                return pregraph_timing;
            }
            slot.pregraph_gpu_timing_pending.clear();
            (**encoder).reset_query_set(slot.pregraph_gpu_timing_query_set, 0, kPregraphGpuTimingQueryCount);
            (**encoder).write_timestamp(RHI::PipelineStage::AllCommands, slot.pregraph_gpu_timing_query_set, 0);
        }
        if (Core::RendererResult spectral_scene = prepare_spectral_scene_acceleration_structure(
                **encoder, slot, submission);
            !spectral_scene.has_value()) {
            return spectral_scene;
        }
        if (gpu_timing_enabled) {
            (**encoder).write_timestamp(RHI::PipelineStage::AllCommands, slot.pregraph_gpu_timing_query_set, 1);
            slot.pregraph_gpu_timing_pending.push_back(RenderGraph::GpuPassTiming{
                .label = UString{"pre-graph: TLAS build"_ustr},
                .begin_query_index = 0,
                .end_query_index = 1,
            });
        }
        // Whether the caustic photon map (up to 262144 photons, each up to max_bounces ray-traced
        // segments) needs re-emitting this frame. Gated on spectral_photon_signature (geometry + sun +
        // photon settings only, no camera transform — see its own doc comment above), NOT
        // spectral_accumulation_reset, so a static-geometry camera pan/rotate reuses this slot's
        // existing photon map instead of paying full re-emission every single frame. Computed here
        // (before prepare_spectral_photon_mapping can flip `populated` to true) so every later gate in
        // this function sees the same pre-frame state.
        const bool spectral_photon_mapping = full_path_tracing &&
            submission.render_graph.spectral_path_tracing.photon_count > 0u;
        const bool spectral_photon_emission_needed = spectral_photon_mapping &&
            (!slot.spectral_photon_targets.populated ||
             slot.spectral_photon_targets.state_signature != spectral_photon_signature);

        // Resolve the expected short-timeout NotReady path before text/UI atlas preparation mutates
        // residency. Once acquired, every later early return is guarded by acquired_image_guard and
        // treated as a real frame failure rather than an intentionally skipped frame.
        optional<RHI::SurfaceTexture> acquired_surface;
        if (!offscreen_output) {
            auto acquired = [&]() {
                ScopedRendererStageTimer timer{"acquire swapchain texture", &current_frame_cpu_stage_timings_ms};
                return device->acquire_next_texture(record.rhi_swapchain, frame_slot_index);
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
            acquired_surface = *acquired;
            acquired_image_guard.record = &record;
            if (acquired_surface->suboptimal) {
                record.rhi_swapchain_dirty = true;
            }
        }

        vector<TextDrawBatch> text_overlay_batches;
        if (submission.render_graph.debug_overlay && submission.render_graph.draw_overlay_text) {
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
                            presentation_extent.x, presentation_extent.y,
                            render_extent.x, render_extent.y, resolution_scale * 100.0f),
                std::format("GPU: {}", overlay_gpu_info ? overlay_gpu_info->name : string{"unknown"}),
                std::format("FPS: {:.1f} ({:.2f} ms)", overlay_fps, frame.delta_seconds * 1000.0),
                std::format("Frame: {}", frame.frame_index),
                [&] {
                    if (!hdr_output) {
                        return offscreen_output
                                   ? string{"HDR: disabled (off-screen SDR)"}
                                   : string{"HDR: disabled (SDR/sRGB)"};
                    }
                    return std::format("HDR: enabled ({})",
                                       hdr_color_space_name(record.presentation.hdr_color_space));
                }(),
                [&] {
                    if (offscreen_output) {
                        return std::format("Output: off-screen SDR target #{}", submission.offscreen_target.value);
                    }
                    const RHI::PresentationResolution presentation =
                        device->presentation_resolution(record.rhi_swapchain);
                    return std::format("Present: {}{}", RHI::present_mode_name(presentation.effective_mode),
                                       presentation.degraded ? " (degraded)" : "");
                }(),
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
                                         glm::vec2{presentation_extent},
                                         slot.text_overlay_resources,
                                         submission.transient_buffers, submission.retired_text_atlas_resources,
                                         text_overlay_batches);
                !text_prepared.has_value()) {
                return text_prepared;
            }
        }

        if (submission.render_graph.ui_overlay) {
            const glm::vec2 ui_viewport_size{presentation_extent};
            if (Core::RendererResult ui_prepared = submission.render_graph.ui_overlay.prepare(
                    *device, **encoder, ui_viewport_size, record.surface, frame_slot_index,
                    submission.transient_buffers, submission.retired_text_atlas_resources);
                !ui_prepared.has_value()) {
                return ui_prepared;
            }
        }

        // Reused across frames (record.graph, a WindowSurfaceRecord member) rather than a fresh
        // stack-local object. reset() retains the graph's outer resource/pass container capacities;
        // individual pass builders still rebuild their own labels, attachment vectors, and callbacks.
        // The adjacent semantic blackboard also retains its small entry-vector allocation.
        RenderGraph &graph = record.graph;
        graph.reset();
        RenderGraphBlackboard &graph_resources = record.graph_resources;
        graph_resources.reset();


        const RHI::TextureHandle output_texture = offscreen_output
            ? resolved_offscreen->texture
            : acquired_surface->texture;
        const RHI::TextureViewHandle output_view = offscreen_output
            ? resolved_offscreen->view
            : acquired_surface->view;

        // Deliberately NOT hoisted above acquire with the TLAS work:
        // prepare_spectral_photon_mapping marks
        // slot.spectral_photon_targets.populated = true (and updates its state_signature) as soon as
        // it *records* the emission dispatch, optimistically assuming the encoder recording it will
        // actually be submitted. Recording it before acquire would mean the routine, expected-to-
        // happen "NotReady" acquire result (a clean early return, not an error -- see below) could
        // mark the photon map populated for a frame whose GPU work never ran, leaving a later frame
        // that trusts `populated` reading stale/uninitialized photon data. TLAS build has no such
        // optimistic-completion bookkeeping (it unconditionally rebuilds every call), so it stays
        // safely hoisted above; only this call needs to wait until an image is actually in hand.
        if (gpu_timing_enabled) {
            (**encoder).write_timestamp(RHI::PipelineStage::AllCommands, slot.pregraph_gpu_timing_query_set, 2);
        }
        if (Core::RendererResult photon_mapping = prepare_spectral_photon_mapping(
                **encoder, slot, submission, spectral_photon_emission_needed, spectral_photon_signature);
            !photon_mapping.has_value()) {
            return photon_mapping;
        }
        if (gpu_timing_enabled) {
            (**encoder).write_timestamp(RHI::PipelineStage::AllCommands, slot.pregraph_gpu_timing_query_set, 3);
            slot.pregraph_gpu_timing_pending.push_back(RenderGraph::GpuPassTiming{
                .label = UString{"pre-graph: photon hash clear"_ustr},
                .begin_query_index = 2,
                .end_query_index = 3,
            });
        }

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
        const RenderGraphTextureHandle final_output = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = output_texture,
            .default_view = output_view,
            .format = output_format,
            .extent = RHI::Extent3D{
                .width = presentation_extent.x,
                .height = presentation_extent.y,
                .depth_or_layers = 1,
            },
            .usage = offscreen_output
                ? RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                      RHI::TextureUsage::TransferSrc
                : RHI::TextureUsage::ColorAttachment,
            .initial_layout = offscreen_output && resolved_offscreen->initialized
                ? RHI::TextureLayout::ShaderReadOnly
                : RHI::TextureLayout::Undefined,
            .initial_stage = offscreen_output && resolved_offscreen->initialized
                ? RHI::PipelineStage::AllGraphics | RHI::PipelineStage::ComputeShader
                : RHI::PipelineStage::None,
            .initial_access = offscreen_output && resolved_offscreen->initialized
                ? RHI::AccessFlags::ShaderRead
                : RHI::AccessFlags::None,
            .final_layout = offscreen_output
                ? RHI::TextureLayout::ShaderReadOnly
                : RHI::TextureLayout::Present,
            .final_stage = offscreen_output
                ? RHI::PipelineStage::AllGraphics | RHI::PipelineStage::ComputeShader
                : RHI::PipelineStage::None,
            .final_access = offscreen_output ? RHI::AccessFlags::ShaderRead : RHI::AccessFlags::None,
            .label = offscreen_output ? "off-screen final color" : "swapchain color",
        });
        graph.mark_output(final_output);
        const RHI::Extent3D frame_extent{.width = render_extent.x, .height = render_extent.y, .depth_or_layers = 1};
        const RenderGraphTextureHandle gbuffer_albedo = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.gbuffer_albedo,
            .default_view = slot.deferred_targets.gbuffer_albedo_view,
            .format = submission.deferred_formats.albedo,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
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
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
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
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer material",
        });
        const RenderGraphTextureHandle gbuffer_emissive = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.gbuffer_emissive,
            .default_view = slot.deferred_targets.gbuffer_emissive_view,
            .format = submission.deferred_formats.emissive,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer emissive",
        });
        const RenderGraphTextureHandle gbuffer_motion = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.motion,
            .default_view = slot.deferred_targets.motion_view,
            .format = submission.deferred_formats.motion,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred gbuffer motion",
        });
        // HDR scene-color target consumed by gizmos and post-processing.
        const RenderGraphTextureHandle scene_color = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.scene_color,
            .default_view = slot.deferred_targets.scene_color_view,
            .format = submission.deferred_formats.scene_color,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc,
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
        const RenderGraphTextureHandle spectral_effect = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "spectral hybrid effect",
        });
        const RenderGraphTextureHandle spectral_primary_depth = graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::R32Float,
            .extent = frame_extent,
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "spectral primary depth",
        });
        RenderGraphTextureHandle spectral_accumulation{};
        if (full_path_tracing) {
            const bool history_initialized = record.spectral_accumulation.initialized;
            spectral_accumulation = graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = record.spectral_accumulation.texture,
                .default_view = record.spectral_accumulation.view,
                .format = RHI::Format::RGBA32Float,
                .extent = frame_extent,
                .usage = RHI::TextureUsage::Storage,
                .initial_layout = history_initialized
                    ? RHI::TextureLayout::General : RHI::TextureLayout::Undefined,
                .initial_stage = history_initialized
                    ? RHI::PipelineStage::ComputeShader : RHI::PipelineStage::None,
                .initial_access = history_initialized
                    ? RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite : RHI::AccessFlags::None,
                .final_layout = RHI::TextureLayout::General,
                .final_stage = RHI::PipelineStage::ComputeShader,
                .final_access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                .label = "spectral temporal accumulation",
            });
        }
        RenderGraphBufferHandle spectral_photons{};
        RenderGraphBufferHandle spectral_photon_count{};
        RenderGraphBufferHandle spectral_photon_hash_heads{};
        if (spectral_photon_mapping) {
            spectral_photons = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = slot.spectral_photon_targets.photons,
                .size = static_cast<u64>(slot.spectral_photon_targets.photon_capacity) * 48u,
                .label = "spectral caustic photons",
            });
            // Transfer/TransferWrite only holds when prepare_spectral_photon_mapping actually issued
            // this frame's fill_buffer clears (spectral_photon_emission_needed) — on a skipped frame
            // nothing touches these buffers before the graph runs, same as the photons buffer above,
            // and the ring-buffered FrameInFlight fence wait already makes last emission's writes
            // visible without an additional barrier here.
            spectral_photon_count = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = slot.spectral_photon_targets.valid_count,
                .size = sizeof(u32),
                .initial_stage = spectral_photon_emission_needed
                    ? RHI::PipelineStage::Transfer : RHI::PipelineStage::None,
                .initial_access = spectral_photon_emission_needed
                    ? RHI::AccessFlags::TransferWrite : RHI::AccessFlags::None,
                .label = "spectral caustic photon count",
            });
            spectral_photon_hash_heads = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = slot.spectral_photon_targets.hash_heads,
                .size = static_cast<u64>(slot.spectral_photon_targets.hash_capacity) * sizeof(u32),
                .initial_stage = spectral_photon_emission_needed
                    ? RHI::PipelineStage::Transfer : RHI::PipelineStage::None,
                .initial_access = spectral_photon_emission_needed
                    ? RHI::AccessFlags::TransferWrite : RHI::AccessFlags::None,
                .label = "spectral caustic photon hash heads",
            });
        }
        const RHI::TextureHandle hiz_pyramid_gpu_texture = record.hiz_pyramid.texture;
        const RHI::TextureViewHandle hiz_pyramid_full_view = record.hiz_pyramid.full_view;
        const Core::Extent2D hiz_pyramid_extent = record.hiz_pyramid.extent;
        const u32 hiz_pyramid_mip_levels = record.hiz_pyramid.mip_levels;
        // hiz_cull_input.valid (captured earlier, before this frame's own "hiz build" passes below
        // run) is exactly "was this texture left ShaderReadOnly by last frame's build pass" —
        // ShaderReadOnly preserves contents across the transition this import performs; Undefined is
        // a discard, correct for a texture that has never been written (first frame / just resized —
        // see ensure_hiz_pyramid). Left ShaderReadOnly at frame exit (not whatever the build passes'
        // own last write would otherwise leave it) so *next* frame's "gpu instance cull" pass can
        // read it without an extra transition of its own.
        const RenderGraphTextureHandle hiz_pyramid_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = hiz_pyramid_gpu_texture,
            .default_view = hiz_pyramid_full_view,
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = hiz_pyramid_extent.x, .height = hiz_pyramid_extent.y, .depth_or_layers = 1},
            .mip_levels = hiz_pyramid_mip_levels,
            .initial_layout = hiz_cull_input.valid ? RHI::TextureLayout::ShaderReadOnly : RHI::TextureLayout::Undefined,
            .initial_stage = hiz_cull_input.valid ? RHI::PipelineStage::ComputeShader : RHI::PipelineStage::None,
            .initial_access = hiz_cull_input.valid ? RHI::AccessFlags::ShaderRead : RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::ShaderReadOnly,
            .final_stage = RHI::PipelineStage::ComputeShader,
            .final_access = RHI::AccessFlags::ShaderRead,
            .label = "hi-z pyramid",
        });
        // Hi-Z is consumed by the next frame rather than the current presentation chain.
        graph.mark_output(hiz_pyramid_texture);
        // SRAA keeps only visibility at the requested MSAA rate. Material attributes, motion,
        // lighting, and every post-process remain single-sampled; the multisampled depth is consumed
        // by a depth-guided reconstruction pass after deferred lighting.
        RenderGraphTextureHandle raster_depth = depth_texture;
        const bool multisampled = framebuffer_samples != RHI::SampleCount::X1;
        if (multisampled) {
            raster_depth = graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = slot.deferred_targets.msaa_depth,
                .default_view = slot.deferred_targets.msaa_depth_view,
                .format = submission.deferred_formats.depth,
                .extent = frame_extent,
                .samples = framebuffer_samples,
                .initial_layout = RHI::TextureLayout::Undefined,
                .label = "multisampled deferred visibility depth",
            });
        }

        RenderGraphModuleBuildContext module_context{
            .graph = graph,
            .resources = graph_resources,
            .render_extent = render_extent,
            .presentation_extent = presentation_extent,
        };
        vector<RenderGraphTextureHandle> logical_graph_textures(
            submission.render_graph.custom_graph.texture_count);
        const auto map_logical_texture = [&logical_graph_textures](
                                             LogicalRenderGraphTexture logical,
                                             RenderGraphTextureHandle concrete) {
            if (logical && logical.index < logical_graph_textures.size()) {
                logical_graph_textures[logical.index] = concrete;
            }
        };
        map_logical_texture(submission.render_graph.custom_graph.deferred_scene_output, scene_color);
        graph_resources.publish_texture<RenderGraphSemantics::SceneHdrColor>(scene_color);
        graph_resources.publish_texture<RenderGraphSemantics::ResolvedSceneDepth>(depth_texture);
        graph_resources.publish_texture<RenderGraphSemantics::RasterVisibilityDepth>(raster_depth);
        graph_resources.publish_texture<RenderGraphSemantics::PresentationTarget>(final_output);
        if (submission.deferred_formats.emissive == submission.deferred_formats.scene_color) {
            // Deferred lighting is the last consumer of emissive. SRAA can overwrite the allocation
            // afterward instead of allocating another full-resolution HDR texture.
            graph_resources.publish_texture<RenderGraphSemantics::ReusableSceneHdrScratch>(gbuffer_emissive);
        }

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

        // Sky/atmosphere LUT bakes have no dependency on culling/gbuffer state (only camera height +
        // sun angle), so they're declared early and rebaked every frame — see
        // Renderer::record_atmosphere_lut_bakes' own doc comment for why nothing here is cached.
        RenderGraphTextureHandle transmittance_lut{};
        RenderGraphTextureHandle multi_scattering_lut{};
        RenderGraphTextureHandle sky_view_lut{};
        if (submission.render_graph.render_scene) {
            if (Core::RendererResult atmosphere_luts = record_atmosphere_lut_bakes(
                    graph, slot.atmosphere_targets.constants_buffer, transmittance_lut, multi_scattering_lut,
                    sky_view_lut, submission.transient_bind_groups);
                !atmosphere_luts.has_value()) {
                return atmosphere_luts;
            }
        }

        RenderGraphBufferHandle instance_indirect_commands{};
        RenderGraphBufferHandle compacted_instance_indices{};
        if (!instanced_batches.empty()) {
            instance_indirect_commands = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = instance_cull_resources.indirect_commands_buffer,
                .size = instance_cull_resources.indirect_commands_capacity * sizeof(GpuDrawIndexedIndirectCommand),
                .initial_stage = RHI::PipelineStage::DrawIndirect,
                .initial_access = RHI::AccessFlags::IndirectCommandRead,
                .label = "GPU-culling indirect commands",
            });
            compacted_instance_indices = graph.import_buffer(RenderGraphImportedBufferDesc{
                .buffer = instance_cull_resources.compacted_indices_buffer,
                .size = instance_cull_resources.compacted_indices_capacity * sizeof(u32),
                .initial_stage = RHI::PipelineStage::VertexShader,
                .initial_access = RHI::AccessFlags::ShaderRead,
                .label = "GPU-culling compacted instance indices",
            });
            graph.add_compute_pass("gpu instance cull"_ustr)
                .add_sampled_texture(hiz_pyramid_texture)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = instance_indirect_commands,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = compacted_instance_indices,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .set_execute([this, &submission, &instanced_batches, &instance_cull_resources, &hiz_cull_input](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_instance_cull(
                        context.compute_pass(), instanced_batches, submission.view_projection,
                        submission.camera.world_position, hiz_cull_input, instance_cull_resources,
                        submission.transient_bind_groups);
                });
        }

        if (submission.render_graph.render_scene && !full_path_tracing) {
            if (shadow_frame.atlas_used) {
                // Decided once, here, rather than inside the execute_ callback below: Vulkan's
                // vkCmdBeginRendering must be told up front (VkRenderingInfo's
                // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT flag) whether this render-pass
                // instance will use execute_bundles — that flag has to match what the callback
                // actually does exactly, including on the "too few views to bother parallelizing"
                // fallback (which stays fully inline, no bundles, matching allow_bundles=false there).
                // See RenderGraphRenderPassBuilder::set_allow_bundles' own doc comment for the crash
                // this fixes (VUID-vkCmdExecuteCommands-flags-06024, hit and root-caused this session).
                const bool shadow_atlas_uses_bundles =
                    shadow_frame.render_views.size() >= 2 && Async::Scheduler::worker_count() > 1;
                graph.add_render_pass("raster shadow atlas"_ustr)
                    .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                        .texture = shadow_atlas,
                        .depth_load_op = RHI::LoadOp::Clear,
                        .depth_store_op = RHI::StoreOp::Store,
                        .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                    })
                    .set_render_area(RHI::Rect2D{.x = 0, .y = 0,
                                                 .width = slot.shadow_targets.atlas_size,
                                                 .height = slot.shadow_targets.atlas_size})
                    .set_allow_bundles(shadow_atlas_uses_bundles)
                    .set_execute([this, &submission, &shadow_frame, &slot, frame, shadow_atlas_uses_bundles](
                                     RenderGraphContext &context) -> Core::RendererResult {
                        RHI::RenderPassEncoder &pass = context.render_pass();
                        const f32 shadow_depth_bias = std::isfinite(submission.render_graph.shadow_depth_bias)
                                                          ? std::max(submission.render_graph.shadow_depth_bias, 0.0f)
                                                          : 0.75f;
                        const f32 shadow_slope_bias = std::isfinite(submission.render_graph.shadow_slope_bias)
                                                          ? std::max(submission.render_graph.shadow_slope_bias, 0.0f)
                                                          : 1.0f;
                        const span<const ShadowRenderView> views{shadow_frame.render_views.data(),
                                                                  shadow_frame.render_views.size()};
                        const RHI::Format depth_format = slot.shadow_targets.format;
                        const u32 worker_count = Async::Scheduler::worker_count();

                        // Below this many views, or with no worker pool to spread them across,
                        // per-chunk RenderBundleEncoder overhead isn't worth it — record every view
                        // directly into the primary pass encoder, exactly like before this session's
                        // shadow-parallelization pass (same "not worth it below N" reasoning as
                        // record_render_items_culled's own kParallelRecordThreshold). Branches on the
                        // exact same `shadow_atlas_uses_bundles` value the pass was declared with
                        // (rather than recomputing an equivalent condition here) so this can never drift
                        // out of sync with the allow_bundles flag the render pass was opened with.
                        if (!shadow_atlas_uses_bundles) {
                            return record_shadow_view_chunk(pass, views, submission.draws, depth_format,
                                                            frame.frame_index, shadow_depth_bias, shadow_slope_bias);
                        }

                        RHI::RhiDevice *device = rhi_device();
                        if (device == nullptr) {
                            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                "Cannot record shadow atlas without an RHI device.");
                        }

                        const usize chunk_count = std::min<usize>(worker_count, views.size());
                        const usize chunk_size = (views.size() + chunk_count - 1) / chunk_count;

                        struct ShadowChunkResult {
                            Core::RendererResult status{};
                            RHI::RenderBundleHandle bundle{};
                            unique_ptr<RHI::RenderBundleEncoder> encoder;
                        };
                        vector<ShadowChunkResult> results(chunk_count);

                        // Pool/buffer *creation* happens here, serially, on this thread — not inside
                        // the spawned tasks below. Same root cause and fix as execute_parallel's own
                        // command-pool-creation race found earlier this session (see memory
                        // project_render_threading): calling vkCreateCommandPool/vkAllocateCommandBuffers
                        // concurrently from multiple worker threads reliably corrupts the heap on this
                        // RADV setup, even though each thread creates its own independent pool/buffer —
                        // create_render_bundle_encoder has the exact same pool+buffer creation shape
                        // (VulkanRhiBridgeCommands.cpp) and was never covered by that earlier fix, since
                        // nothing had called it concurrently until this shadow-parallelization pass (the
                        // pre-existing record_render_items_culled >128-item bundle path has the same
                        // latent bug — see that function's own fix in this same change). Only the actual
                        // *recording* (culling + draws) is dispatched to worker threads below.
                        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
                            const usize begin = chunk * chunk_size;
                            const usize end = std::min(views.size(), begin + chunk_size);
                            if (begin >= end) {
                                continue;
                            }
                            const RHI::RenderBundleDesc bundle_desc{
                                .color_formats = {},
                                .depth_stencil_format = depth_format,
                                .samples = RHI::SampleCount::X1,
                                .view_mask = 0,
                                .label = "shadow view chunk",
                            };
                            auto encoder = device->create_render_bundle_encoder(bundle_desc);
                            if (!encoder) {
                                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                                    "Cannot create shadow bundle encoder.");
                            }
                            results[chunk].encoder = std::move(*encoder);
                        }

                        vector<Async::TaskHandle<void>> tasks;
                        tasks.reserve(chunk_count);
                        for (usize chunk = 0; chunk < chunk_count; ++chunk) {
                            const usize begin = chunk * chunk_size;
                            const usize end = std::min(views.size(), begin + chunk_size);
                            if (begin >= end) {
                                continue;
                            }
                            tasks.push_back(Async::Scheduler::spawn([this, &submission, &results, chunk, views, begin,
                                                                      end, depth_format, frame_index = frame.frame_index,
                                                                      shadow_depth_bias, shadow_slope_bias]() {
                                RHI::RenderBundleEncoder &encoder = *results[chunk].encoder;
                                Core::RendererResult recorded = record_shadow_view_chunk(
                                    encoder, views.subspan(begin, end - begin), submission.draws, depth_format,
                                    frame_index, shadow_depth_bias, shadow_slope_bias);
                                if (!recorded.has_value()) {
                                    results[chunk].status = recorded;
                                    return;
                                }
                                auto finished = encoder.finish();
                                if (!finished) {
                                    results[chunk].status =
                                        unexpected(graphics_error_from_rhi(finished.error(), "finish shadow bundle"));
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
                        for (ShadowChunkResult &result : results) {
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
                        // Same use-after-free risk as record_render_items_culled's parallel path (see
                        // its own comment): execute_bundles only records a reference for the GPU to run
                        // later, so destruction must wait for this frame's fence, not happen here.
                        submission.transient_render_bundles.insert(submission.transient_render_bundles.end(),
                                                                    bundles.begin(), bundles.end());
                        if (has_error) {
                            return first_error;
                        }
                        return {};
                    });
            }

            // Z prepass: writes real depth for every surviving (alpha-tested-or-not) fragment before
            // any color shading happens, so "deferred gbuffer geometry" below can require an exact
            // depth match instead of writing depth itself — a fragment that isn't the visible surface
            // never runs full PBR shading, eliminating occluded-fragment overdraw cost. See
            // Renderer::depth_only_pipeline_for's doc comment.
            //
            // allow_bundles must be decided here, before the pass is opened, not inside the
            // execute_ callback below — same reasoning as the raster shadow atlas pass's
            // shadow_atlas_uses_bundles (see that pass's own comment for the crash this avoids,
            // VUID-vkCmdExecuteCommands-flags-06024). Counts survivors against
            // kParallelRecordThreshold up front (a cheap frustum test, not the actual recording
            // work) rather than using submission.draws.size() outright, so a scene with many total
            // items but heavy frustum culling (e.g. a large open world) doesn't pay bundle-encoder
            // overhead for a handful of visible draws.
            usize zprepass_visible_count = 0;
            for (const RenderItem &item : submission.draws) {
                if (render_item_visible(item, camera_frustum)) {
                    ++zprepass_visible_count;
                }
            }
            const bool zprepass_uses_bundles =
                zprepass_visible_count >= kParallelRecordThreshold && Async::Scheduler::worker_count() > 1;
            graph.add_render_pass("z prepass"_ustr)
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = raster_depth,
                    .depth_load_op = RHI::LoadOp::Clear,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_allow_bundles(zprepass_uses_bundles)
                .set_execute([this, &submission, render_extent, frame, camera_frustum,
                              framebuffer_samples, zprepass_uses_bundles](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.x),
                        .height = static_cast<f32>(render_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y});
                    return record_render_items_culled(pass, submission.draws, camera_frustum,
                                                       span<const RHI::Format>{}, submission.deferred_formats.depth,
                                                       frame.frame_index, submission.view_projection,
                                                       /*depth_only=*/true, /*standard_depth_test=*/false, "z prepass",
                                                       zprepass_uses_bundles, submission.transient_render_bundles,
                                                       /*shadow_map=*/false, 0.0f, 0.0f, framebuffer_samples);
                });

            // Same allow_bundles-decided-before-declaration reasoning as "z prepass" above, against
            // gbuffer_draws (the individually-drawn subset — instanced batches are recorded
            // separately below via record_instanced_batches, not through this bundle path).
            usize gbuffer_visible_count = 0;
            for (const RenderItem &item : gbuffer_draws) {
                if (render_item_visible(item, camera_frustum)) {
                    ++gbuffer_visible_count;
                }
            }
            const bool gbuffer_uses_bundles =
                gbuffer_visible_count >= kParallelRecordThreshold && Async::Scheduler::worker_count() > 1;
            RenderGraphRenderPassBuilder &gbuffer_pass = graph.add_render_pass("deferred gbuffer geometry"_ustr);
            gbuffer_pass.add_color_attachment(RenderGraphColorAttachmentDesc{
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
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = gbuffer_emissive,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                })
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = gbuffer_motion,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
                })
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    // At 1x this is the same target populated by the Z prepass. Under SRAA the
                    // visibility prepass is a separate MSAA image, so clear and establish 1x depth
                    // alongside the 1x G-buffer instead of resolving multisampled depth.
                    .depth_load_op = multisampled ? RHI::LoadOp::Clear : RHI::LoadOp::Load,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_allow_bundles(gbuffer_uses_bundles);
            if (!instanced_batches.empty()) {
                gbuffer_pass
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = instance_indirect_commands,
                        .stages = RHI::PipelineStage::DrawIndirect,
                        .access = RHI::AccessFlags::IndirectCommandRead,
                    })
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = compacted_instance_indices,
                        .stages = RHI::PipelineStage::VertexShader,
                        .access = RHI::AccessFlags::ShaderRead,
                    });
            }
            gbuffer_pass.set_execute([this, &submission, render_extent, frame, camera_frustum, gbuffer_draws, &instanced_batches,
                             &instance_cull_resources, multisampled, object_history_group,
                             gbuffer_uses_bundles](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.x),
                        .height = static_cast<f32>(render_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y});
                    const array<RHI::Format, 5> gbuffer_formats{
                        submission.deferred_formats.albedo,
                        submission.deferred_formats.normal,
                        submission.deferred_formats.material,
                        submission.deferred_formats.emissive,
                        submission.deferred_formats.motion,
                    };
                    const span<const RHI::Format> gbuffer_formats_span{gbuffer_formats.data(), gbuffer_formats.size()};
                    if (Core::RendererResult recorded = record_render_items_culled(
                            pass, gbuffer_draws, camera_frustum, gbuffer_formats_span, submission.deferred_formats.depth,
                            frame.frame_index, submission.view_projection, /*depth_only=*/false,
                            /*standard_depth_test=*/multisampled, "deferred gbuffer geometry",
                            gbuffer_uses_bundles, submission.transient_render_bundles,
                            /*shadow_map=*/false, 0.0f, 0.0f, RHI::SampleCount::X1,
                            /*with_object_history=*/true, object_history_group);
                        !recorded.has_value()) {
                        return recorded;
                    }
                    if (!instanced_batches.empty()) {
                        if (Core::RendererResult recorded_instanced = record_instanced_batches(
                                pass, instanced_batches, gbuffer_formats_span, submission.deferred_formats.depth,
                                frame.frame_index, submission.view_projection, submission.camera.previous_view_projection,
                                instance_cull_resources, submission.transient_bind_groups, RHI::SampleCount::X1);
                            !recorded_instanced.has_value()) {
                            return recorded_instanced;
                        }
                    }
                    return {};
                });

            // Builds *this* frame's Hi-Z pyramid from the depth the passes above just finished
            // writing, for *next* frame's "gpu instance cull" pass to occlusion-test against — see
            // HiZPyramidTargets's and record_hiz_build's own doc comments for why it can't be this
            // same frame's cull pass instead.
            if (Core::RendererResult hiz_built = record_hiz_build(
                    graph, depth_texture, slot.deferred_targets.depth_view, render_extent,
                    hiz_pyramid_texture, record.hiz_pyramid, submission.transient_bind_groups);
                !hiz_built.has_value()) {
                return hiz_built;
            }
        }

        const SpectralRenderMode spectral_mode = submission.render_graph.spectral_path_tracing.mode;
        const bool hybrid_spectral = submission.render_graph.render_scene &&
                                     spectral_mode != SpectralRenderMode::RasterDeferred &&
                                     spectral_mode != SpectralRenderMode::FullPathTracing;
        if (hybrid_spectral) {
            graph.add_compute_pass("hybrid spectral ray query"_ustr)
                .add_sampled_texture(gbuffer_albedo)
                .add_sampled_texture(gbuffer_normal)
                .add_sampled_texture(gbuffer_material)
                .add_sampled_texture(gbuffer_emissive)
                .add_sampled_texture(gbuffer_motion)
                .add_sampled_texture(depth_texture)
                .add_sampled_texture(transmittance_lut)
                .add_sampled_texture(sky_view_lut)
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_effect})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, gbuffer_emissive, gbuffer_motion, depth_texture,
                              transmittance_lut, sky_view_lut,
                              spectral_effect](RenderGraphComputeContext &context) -> Core::RendererResult {
                    const RHI::TextureViewHandle output = context.texture(spectral_effect).default_view;
                    return record_spectral_integrator(
                        context.compute_pass(), slot, submission, render_extent,
                        SpectralIntegratorViews{
                            .raster_albedo = context.texture(gbuffer_albedo).default_view,
                            .raster_normal = context.texture(gbuffer_normal).default_view,
                            .raster_material = context.texture(gbuffer_material).default_view,
                            .raster_emissive = context.texture(gbuffer_emissive).default_view,
                            .raster_motion = context.texture(gbuffer_motion).default_view,
                            .raster_depth = context.texture(depth_texture).default_view,
                            .effect_output = output,
                            .scene_color_output = output,
                            .gbuffer_motion_output = output,
                            .primary_depth_output = output,
                            .accumulation_output = output,
                            .transmittance_lut = context.texture(transmittance_lut).default_view,
                            .sky_view_lut = context.texture(sky_view_lut).default_view,
                            .atmosphere_constants = slot.atmosphere_targets.constants_buffer,
                        }, false);
                });
        }

        if (spectral_photon_emission_needed) {
            graph.add_compute_pass("spectral photon emission"_ustr)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photons,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderWrite,
                    .read = false,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_count,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .set_execute([this, &slot, &submission](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_spectral_photon_emission(context.compute_pass(), slot, submission);
                });

            graph.add_compute_pass("spectral photon spatial hash"_ustr)
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photons,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_count,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead,
                    .read = true,
                    .write = false,
                })
                .add_buffer(RenderGraphBufferAccessDesc{
                    .buffer = spectral_photon_hash_heads,
                    .stages = RHI::PipelineStage::ComputeShader,
                    .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite,
                    .read = true,
                    .write = true,
                })
                .set_execute([this, &slot, &submission](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    return record_spectral_photon_hash(context.compute_pass(), slot, submission);
                });
        }

        if (submission.render_graph.render_scene && full_path_tracing) {
            RenderGraphComputePassBuilder &full_path_pass = graph.add_compute_pass("full spectral path tracing"_ustr);
            full_path_pass
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_effect})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = scene_color})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = gbuffer_motion})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = spectral_primary_depth})
                .add_storage_texture(RenderGraphStorageTextureAccessDesc{
                    .texture = spectral_accumulation,
                    .read = true,
                    .write = true,
                })
                .add_sampled_texture(transmittance_lut)
                .add_sampled_texture(sky_view_lut);
            if (spectral_photon_mapping) {
                full_path_pass
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photons,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    })
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photon_count,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    })
                    .add_buffer(RenderGraphBufferAccessDesc{
                        .buffer = spectral_photon_hash_heads,
                        .stages = RHI::PipelineStage::ComputeShader,
                        .access = RHI::AccessFlags::ShaderRead,
                        .read = true,
                        .write = false,
                    });
            }
            full_path_pass.set_execute([this, &submission, &slot, render_extent, spectral_effect, scene_color,
                              gbuffer_motion, spectral_primary_depth, spectral_accumulation,
                              transmittance_lut, sky_view_lut,
                              spectral_accumulation_reset](
                                 RenderGraphComputeContext &context) -> Core::RendererResult {
                    const RHI::TextureViewHandle dummy = context.texture(spectral_effect).default_view;
                    return record_spectral_integrator(
                        context.compute_pass(), slot, submission, render_extent,
                        SpectralIntegratorViews{
                            .raster_albedo = dummy,
                            .raster_normal = dummy,
                            .raster_material = dummy,
                            .raster_emissive = dummy,
                            .raster_motion = dummy,
                            .raster_depth = dummy,
                            .effect_output = dummy,
                            .scene_color_output = context.texture(scene_color).default_view,
                            .gbuffer_motion_output = context.texture(gbuffer_motion).default_view,
                            .primary_depth_output = context.texture(spectral_primary_depth).default_view,
                            .accumulation_output = context.texture(spectral_accumulation).default_view,
                            .transmittance_lut = context.texture(transmittance_lut).default_view,
                            .sky_view_lut = context.texture(sky_view_lut).default_view,
                            .atmosphere_constants = slot.atmosphere_targets.constants_buffer,
                        }, spectral_accumulation_reset);
                });

            graph.add_render_pass("path traced depth commit"_ustr)
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    .depth_load_op = RHI::LoadOp::DontCare,
                    .depth_store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = spectral_primary_depth})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0,
                                             .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &slot, spectral_primary_depth, render_extent](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    return record_spectral_depth_commit(
                        context.render_pass(), slot,
                        context.texture(spectral_primary_depth).default_view, render_extent);
                });
        }

        if (submission.render_graph.render_scene && !full_path_tracing) {
            const RenderGraphTextureHandle lighting_spectral_effect = hybrid_spectral
                ? spectral_effect : gbuffer_emissive;
            RenderGraphRenderPassBuilder &lighting_pass = graph.add_render_pass("deferred shadow lighting"_ustr);
            lighting_pass.add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            });
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_albedo});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_normal});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_material});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = gbuffer_emissive});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = depth_texture});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = lighting_spectral_effect});
            if (shadow_frame.atlas_used) {
                lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = shadow_atlas});
            }
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = transmittance_lut});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = multi_scattering_lut});
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = sky_view_lut});
            lighting_pass
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, gbuffer_emissive, depth_texture, lighting_spectral_effect,
                              shadow_atlas, &shadow_frame,
                              transmittance_lut, multi_scattering_lut, sky_view_lut](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.x),
                        .height = static_cast<f32>(render_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0,
                                                 .width = render_extent.x, .height = render_extent.y});
                    const RHI::TextureViewHandle atlas_view = shadow_frame.atlas_used
                        ? context.texture(shadow_atlas).default_view
                        : context.texture(depth_texture).default_view;
                    return record_shadow_lighting(
                        pass,
                        context.texture(gbuffer_albedo).default_view,
                        context.texture(gbuffer_normal).default_view,
                        context.texture(gbuffer_material).default_view,
                        context.texture(gbuffer_emissive).default_view,
                        context.texture(depth_texture).default_view,
                        context.texture(lighting_spectral_effect).default_view,
                        atlas_view,
                        slot.shadow_targets.lighting_buffer,
                        context.texture(transmittance_lut).default_view,
                        context.texture(multi_scattering_lut).default_view,
                        context.texture(sky_view_lut).default_view,
                        slot.atmosphere_targets.constants_buffer,
                        submission.deferred_formats.scene_color,
                        submission.transient_bind_groups);
                });
        } else if (!submission.render_graph.render_scene) {
            // Overlay-only views still need a defined HDR source for gizmos and post-processing.
            graph.add_render_pass("scene background"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = scene_color,
                    .load_op = RHI::LoadOp::Clear,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = RHI::ClearColor{background.r, background.g, background.b, background.a},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x,
                                             .height = render_extent.y});
        }

        if (submission.render_graph.render_scene && multisampled) {
            if (Core::RendererResult reconstructed = build_deferred_msaa_module(
                    module_context, submission, framebuffer_samples);
                !reconstructed.has_value()) {
                return reconstructed;
            }
        }

        const RenderGraphTextureHandle scene_before_bloom =
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>();

        // Always-on debug markers (e.g. light-position icospheres, Shaders/geometry_color.slang).
        // This pass writes into the semantic HDR chain before spatial AA and custom HDR transforms,
        // so marker edges are filtered and emissive values participate in the complete bloom pyramid.
        // When scene geometry ran, load its depth so occluded gizmos stay hidden; otherwise clear
        // depth for a defined overlay-only pass.
        if (!submission.gizmo_draws.empty()) {
            const array<RHI::Format, 1> gizmo_color_formats{submission.deferred_formats.scene_color};
            graph.add_render_pass("pre-bloom light indicators"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = scene_before_bloom,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                    .texture = depth_texture,
                    .depth_load_op = submission.render_graph.render_scene ? RHI::LoadOp::Load : RHI::LoadOp::Clear,
                    .depth_store_op = RHI::StoreOp::Store,
                    .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &submission, render_extent, frame, gizmo_color_formats](
                                 RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f, .y = 0.0f,
                        .width = static_cast<f32>(render_extent.x),
                        .height = static_cast<f32>(render_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y});
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

        if (Core::RendererResult anti_aliased = build_post_process_aa_module(module_context, submission);
            !anti_aliased.has_value()) {
            return anti_aliased;
        }
        map_logical_texture(
            submission.render_graph.custom_graph.anti_aliasing_output,
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>());

        // Custom graph branches are lowered after scene-space gizmos and the complete AA module, so the
        // selected presentation branch and every branch rooted at AA see the same pre-bloom HDR result.
        if (Core::RendererResult effects = build_custom_graph_stage(
                module_context, submission, PostProcessStage::BeforeBloom, logical_graph_textures);
            !effects.has_value()) {
            return effects;
        }

        if (Core::RendererResult bloom = build_bloom_module(
                module_context, submission, slot, bloom_active, bloom_format);
            !bloom.has_value()) {
            return bloom;
        }

        map_logical_texture(
            submission.render_graph.custom_graph.bloom_output,
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>());
        if (Core::RendererResult effects = build_custom_graph_stage(
                module_context, submission, PostProcessStage::AfterBloomBeforeToneMap, logical_graph_textures);
            !effects.has_value()) {
            return effects;
        }

        // Tonemap post-process: sample the final scene-linear HDR result (bloom already composited in
        // above, if active) and resolve it to the swapchain. recreate_rhi_swapchain() above already
        // picks a matching format/color-space for the swapchain itself once presentation.hdr_enabled
        // is set (hdr_presentation_format/hdr_presentation_color_space) — the tonemap pipeline's own
        // color-attachment format must match, and the shader must know which curve to apply: PQ for
        // Hdr10St2084 (and DolbyVision, best-effort — Vulkan applies no fixed-function PQ curve),
        // HLG for Hdr10Hlg, none at all for ScrgbLinear (the OS/compositor tone-maps a linear float
        // target itself), sRGB's automatic OETF for SDR.
        if (Core::RendererResult tone_mapped = build_tonemap_module(
                module_context, submission, output_format, hdr_output, record.presentation.hdr_color_space);
            !tone_mapped.has_value()) {
            return tone_mapped;
        }

        if (submission.render_graph.debug_overlay && submission.render_graph.draw_overlay_text) {
            // Shaping/residency/instance upload happened above; this pass only issues the prepared
            // instanced draws over the tonemapped scene.
            graph.add_render_pass("debug text overlay"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = final_output,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y})
                .set_execute([this, presentation_extent, &text_overlay_batches](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.x),
                        .height = static_cast<f32>(presentation_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y});
                    const glm::vec2 viewport_size{presentation_extent};
                    return draw_text_overlay(pass, text_overlay_batches, viewport_size);
                });
        }

        if (submission.render_graph.ui_overlay) {
            // prepare() already ran above (before this graph's first pass was declared) — this
            // pass only issues the draw calls it prepared, over the fully composited frame.
            graph.add_render_pass("UI overlay"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = final_output,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y})
                .set_execute([presentation_extent, surface = record.surface, frame_slot_index,
                              &submission](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.x),
                        .height = static_cast<f32>(presentation_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = presentation_extent.x, .height = presentation_extent.y});
                    const glm::vec2 viewport_size{presentation_extent};
                    return submission.render_graph.ui_overlay.draw(pass, viewport_size, surface, frame_slot_index);
                });
        }

        if (submission.render_graph.debug_overlay) {
            const f64 seconds = duration<f64>(steady_clock::now() - declare_graph_start).count();
            current_frame_cpu_stage_timings_ms.emplace_back("declare render graph", seconds * 1000.0);
        }
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
        vector<RHI::CommandBufferHandle> frame_command_buffers;
        {
            ScopedRendererStageTimer timer{"execute render graph", &current_frame_cpu_stage_timings_ms};
            // CPU per-pass timing rides the same debug_overlay gate as GPU per-pass timing
            // (gpu_timing_enabled) even though it needs no query set of its own — keeps the two
            // breakdowns' pass lists in lockstep and avoids per-frame vector churn when the overlay
            // (the only current consumer) is off.
            //
            // execute_parallel (not execute()) — takes ownership of `encoder` (already carrying the
            // text-overlay/UI prep work recorded into it above), finishes it as the first command
            // buffer, then records the graph's own passes into their own encoder(s), one per pass for
            // any level with more than one independent pass (Stage 4 of the render-parallelization
            // roadmap — see RenderGraph::execute_parallel's own doc comment). `frame_command_buffers`
            // ends up with every resulting handle in submission order.
            Core::RendererResult graph_result = gpu_timing_enabled
                ? graph.execute_parallel(*device, std::move(*encoder), RHI::QueueLane{}, frame_command_buffers,
                                         slot.gpu_timing.query_set, &slot.gpu_timing.pending,
                                         &slot.cpu_timing.pass_timings)
                : graph.execute_parallel(*device, std::move(*encoder), RHI::QueueLane{}, frame_command_buffers);
            if (!graph_result.has_value()) {
                return graph_result;
            }
            if (gpu_timing_enabled) {
                slot.gpu_timing.has_pending_results = true;
                slot.cpu_timing.has_pending_results = true;
            }
        }

        const span<const RHI::SurfaceTexture> presented_textures = acquired_surface
            ? span<const RHI::SurfaceTexture>{&*acquired_surface, 1}
            : span<const RHI::SurfaceTexture>{};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{frame_command_buffers.data(), frame_command_buffers.size()},
            .waits = {},
            .signals = {},
            .presented_textures = presented_textures,
            .fence = slot.fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer frame submit",
        };
        {
            ScopedRendererStageTimer timer{"submit RHI frame", &current_frame_cpu_stage_timings_ms};
            if (auto submitted = device->submit(submit_desc); !submitted) {
                graph.destroy_transient_resources(*device);
                for (RHI::CommandBufferHandle command_buffer : frame_command_buffers) {
                    device->destroy_command_buffer(command_buffer);
                }
                return unexpected(graphics_error_from_rhi(submitted.error(), "submit RHI frame"));
            }
        }
        // The acquisition semaphore (if any) is now safely consumed -- submit_desc's presented_textures
        // embedded it as a wait-semaphore, and that submission just succeeded. See acquired_image_guard's
        // own doc comment: only a *successful* submit() actually resolves the acquired image.
        acquired_image_guard.resolved = true;

        // The frame is now in flight. Hand its GPU resources to the ring slot for fence-gated cleanup —
        // deliberately NO wait here (the whole point of the async model). They are reclaimed the next time
        // this slot comes round, after its fence has signaled.
        slot.command_buffers = std::move(frame_command_buffers);
        slot.transient_textures = std::move(submission.retired_text_atlas_resources.textures);
        slot.transient_texture_views = std::move(submission.retired_text_atlas_resources.texture_views);
        graph.take_transient_resources(slot.transient_textures, slot.transient_texture_views);
        slot.transient_bind_groups.insert(slot.transient_bind_groups.end(),
                                          submission.transient_bind_groups.begin(),
                                          submission.transient_bind_groups.end());
        slot.transient_buffers.insert(slot.transient_buffers.end(),
                                      submission.transient_buffers.begin(),
                                      submission.transient_buffers.end());
        slot.transient_render_bundles.insert(slot.transient_render_bundles.end(),
                                             submission.transient_render_bundles.begin(),
                                             submission.transient_render_bundles.end());
        submission.transient_bind_groups.clear();
        submission.transient_buffers.clear();
        submission.transient_render_bundles.clear();
        slot.submitted = true;
        if (full_path_tracing) {
            record.spectral_accumulation.initialized = true;
            record.spectral_accumulation.state_signature = spectral_accumulation_signature;
            record.spectral_accumulation.last_frame_index = frame.frame_index;
        }

        if (offscreen_output) {
            mark_offscreen_render_target_initialized(submission.offscreen_target);
        } else {
            // Handed off to Renderer's shared PresentationCoordinator rather than called
            // synchronously here: on Windows the driver commonly blocks the calling thread inside
            // vkQueuePresentKHR until this frame's GPU work actually finishes (present-mode
            // independent -- its pWaitSemaphores waits on the render-finished semaphore this same
            // frame's submit() just signaled, only moments earlier). Blocking this render thread on
            // that would waste the CPU/GPU overlap desired_frames_in_flight is meant to buy, and
            // routing through the coordinator (rather than a per-window present thread) gives every
            // window sharing this native queue one ordered point of issuance instead of N
            // independently-threaded ones with no ordering guarantee relative to each other. The
            // result (error / suboptimal-dirty flag / queue-lock-wait timing) is picked up next frame
            // by drain_pending_present() instead of here -- see WindowSurfaceRecord::pending_present's
            // doc comment for the ordering invariant that keeps this safe.
            ScopedRendererStageTimer timer{"issue present", &current_frame_cpu_stage_timings_ms};
            RHI::FenceHandle completion_fence{};
            if (device->is_enabled(RHI::Feature::SwapchainMaintenance)) {
                auto created_fence = device->create_fence(RHI::FenceDesc{.label = "renderer present completion fence"});
                if (!created_fence) {
                    return unexpected(graphics_error_from_rhi(created_fence.error(), "create present completion fence"));
                }
                completion_fence = *created_fence;
                record.active_presentation_completion_fences.push_back(completion_fence);
                record.pending_present_completion_fence = completion_fence;
            }
            RHI::PresentDesc present_desc{
                .texture = *acquired_surface,
                .completion_fence = completion_fence,
                .label = "renderer present",
            };
            const bool present_via_compute = device->presentation_resolution(record.rhi_swapchain).present_queue_is_compute;
            record.pending_present = presentation_coordinator_for(present_via_compute).enqueue(
                device, present_desc, &record.last_present_lock_wait_ms);
        }

        if (submission.render_graph.wait_for_completion) {
            ScopedRendererStageTimer timer{"wait explicitly requested frame completion", &current_frame_cpu_stage_timings_ms};
            auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true);
            if (!waited) {
                return unexpected(graphics_error_from_rhi(waited.error(), "wait explicitly requested frame completion"));
            }
            if (!*waited) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "wait explicitly requested frame completion: vkWaitForFences timed out.");
            }
            // Explicit completion means presented too, not just GPU-rendered -- drain the present this
            // same call just issued above rather than leaving it for next frame.
            if (Core::RendererResult drained = drain_pending_present(record, &current_frame_cpu_stage_timings_ms);
                !drained.has_value()) {
                return drained;
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
                                                                  const DeferredTargetFormats &formats,
                                                                  RHI::SampleCount samples) {
        ZoneScopedN("Renderer::ensure_frame_deferred_targets");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        const bool matches = slot.deferred_targets.gbuffer_albedo &&
            slot.deferred_targets.depth &&
            slot.deferred_targets.extent.x == extent.x &&
            slot.deferred_targets.extent.y == extent.y &&
            slot.deferred_targets.formats.albedo == formats.albedo &&
            slot.deferred_targets.formats.normal == formats.normal &&
            slot.deferred_targets.formats.material == formats.material &&
            slot.deferred_targets.formats.emissive == formats.emissive &&
            slot.deferred_targets.formats.scene_color == formats.scene_color &&
            slot.deferred_targets.formats.motion == formats.motion &&
            slot.deferred_targets.formats.depth == formats.depth &&
            slot.deferred_targets.samples == samples;
        if (matches) {
            return {};
        }

        // Bloom's cached first-level descriptor references scene_color_view.
        destroy_frame_bloom_targets(slot);
        destroy_frame_deferred_targets(slot);

        auto create_target = [&](RHI::Format format, RHI::TextureUsage usage, const char *label,
                                 RHI::SampleCount target_samples = RHI::SampleCount::X1)
            -> Core::RendererExpected<pair<RHI::TextureHandle, RHI::TextureViewHandle>> {
            auto texture = device->create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = format,
                .extent = RHI::Extent3D{.width = extent.x, .height = extent.y, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = target_samples,
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
            return pair<RHI::TextureHandle, RHI::TextureViewHandle>{*texture, *view};
        };

        constexpr RHI::TextureUsage color_usage = RHI::TextureUsage::ColorAttachment |
                                                  RHI::TextureUsage::Sampled |
                                                  RHI::TextureUsage::Storage |
                                                  RHI::TextureUsage::TransferSrc;
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
        auto emissive = create_target(formats.emissive, color_usage, "persistent deferred gbuffer emissive");
        if (!emissive) {
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(emissive.error());
        }
        auto scene_color = create_target(formats.scene_color, color_usage, "persistent scene color");
        if (!scene_color) {
            device->destroy_texture_view(emissive->second);
            device->destroy_texture(emissive->first);
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(scene_color.error());
        }
        auto motion = create_target(formats.motion, color_usage, "persistent deferred motion");
        if (!motion) {
            device->destroy_texture_view(scene_color->second);
            device->destroy_texture(scene_color->first);
            device->destroy_texture_view(emissive->second);
            device->destroy_texture(emissive->first);
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(motion.error());
        }
        auto depth = create_target(formats.depth,
                                   RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
                                   "persistent deferred depth");
        if (!depth) {
            device->destroy_texture_view(motion->second);
            device->destroy_texture(motion->first);
            device->destroy_texture_view(scene_color->second);
            device->destroy_texture(scene_color->first);
            device->destroy_texture_view(emissive->second);
            device->destroy_texture(emissive->first);
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
            .samples = samples,
            .gbuffer_albedo = albedo->first,
            .gbuffer_albedo_view = albedo->second,
            .gbuffer_normal = normal->first,
            .gbuffer_normal_view = normal->second,
            .gbuffer_material = material->first,
            .gbuffer_material_view = material->second,
            .gbuffer_emissive = emissive->first,
            .gbuffer_emissive_view = emissive->second,
            .motion = motion->first,
            .motion_view = motion->second,
            .scene_color = scene_color->first,
            .scene_color_view = scene_color->second,
            .depth = depth->first,
            .depth_view = depth->second,
        };
        if (samples != RHI::SampleCount::X1) {
            // NVIDIA SRAA stores only subpixel visibility. Unlike a transient resolve source this
            // depth image must survive the prepass and be sampleable by the reconstruction shader.
            auto msaa_depth = create_target(
                formats.depth,
                RHI::TextureUsage::DepthStencilAttachment | RHI::TextureUsage::Sampled,
                "multisampled deferred visibility depth",
                samples);
            if (!msaa_depth) {
                destroy_frame_deferred_targets(slot);
                return unexpected(msaa_depth.error());
            }
            slot.deferred_targets.msaa_depth = msaa_depth->first;
            slot.deferred_targets.msaa_depth_view = msaa_depth->second;
        }
        return {};
    }

    void Renderer::destroy_frame_deferred_targets(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_deferred_targets");
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
            destroy_target(slot.deferred_targets.gbuffer_emissive, slot.deferred_targets.gbuffer_emissive_view);
            destroy_target(slot.deferred_targets.scene_color, slot.deferred_targets.scene_color_view);
            destroy_target(slot.deferred_targets.motion, slot.deferred_targets.motion_view);
            destroy_target(slot.deferred_targets.depth, slot.deferred_targets.depth_view);
            destroy_target(slot.deferred_targets.msaa_depth, slot.deferred_targets.msaa_depth_view);
        }
        slot.deferred_targets = {};
    }

    Core::RendererResult Renderer::ensure_frame_bloom_targets(FrameInFlight &slot,
                                                               Core::Extent2D extent,
                                                               u32 requested_levels,
                                                               f32 downsample_ratio) {
        ZoneScopedN("Renderer::ensure_frame_bloom_targets");
        requested_levels = std::clamp(requested_levels, 1u, 12u);
        downsample_ratio = std::isfinite(downsample_ratio)
            ? std::clamp(downsample_ratio, 1.25f, 2.0f)
            : 1.61803398875f;
        const bool matches = slot.bloom_targets.source_extent == extent &&
            slot.bloom_targets.requested_levels == requested_levels &&
            slot.bloom_targets.downsample_ratio == downsample_ratio &&
            !slot.bloom_targets.textures.empty() &&
            slot.bloom_targets.textures.size() == slot.bloom_targets.extents.size() &&
            slot.bloom_targets.textures.size() == slot.bloom_targets.views.size() &&
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
        slot.bloom_targets.downsample_ratio = downsample_ratio;

        // Hardware mips are locked to powers of two. Independent level images let us use the requested
        // fractional ratio (golden ratio by default), so successive pixel centers do not repeatedly align
        // on a small rational grid. Keep both axes large enough for the
        // 3x3 reconstruction tent; tiny render targets still receive one valid first level.
        constexpr u32 minimum_stable_bloom_axis = 4u;
        Core::Extent2D source_extent = extent;
        for (u32 level = 0; level < requested_levels; ++level) {
            const Core::Extent2D level_extent = glm::max(
                Core::Extent2D{glm::floor(glm::dvec2{source_extent} / static_cast<f64>(downsample_ratio))},
                Core::Extent2D{1u, 1u});
            if (!slot.bloom_targets.extents.empty() &&
                (level_extent.x < minimum_stable_bloom_axis ||
                 level_extent.y < minimum_stable_bloom_axis)) {
                break;
            }
            slot.bloom_targets.extents.push_back(level_extent);
            if (level_extent == source_extent) {
                break;
            }
            source_extent = level_extent;
        }

        slot.bloom_targets.textures.reserve(slot.bloom_targets.extents.size());
        slot.bloom_targets.views.reserve(slot.bloom_targets.extents.size());
        for (const Core::Extent2D level_extent : slot.bloom_targets.extents) {
            auto texture = device->create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = RHI::Format::RG11B10Float,
                .extent = RHI::Extent3D{
                    .width = level_extent.x,
                    .height = level_extent.y,
                    .depth_or_layers = 1,
                },
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .label = "persistent fractional bloom level",
            });
            if (!texture) {
                destroy_frame_bloom_targets(slot);
                return unexpected(graphics_error_from_rhi(texture.error(), "create persistent fractional bloom level"));
            }
            slot.bloom_targets.textures.push_back(*texture);

            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = 0,
                .mip_level_count = 1,
                .label = "persistent fractional bloom level view",
            });
            if (!view) {
                destroy_frame_bloom_targets(slot);
                return unexpected(graphics_error_from_rhi(view.error(), "create persistent fractional bloom level view"));
            }
            slot.bloom_targets.views.push_back(*view);
        }

        u32 image_binding = 0;
        u32 sampler_binding = 0;
        RHI::SamplerHandle sampler{};
        RHI::BindGroupLayoutHandle sampled_layout{};
        {
            auto bloom_guard = bloom_.lock();
            if (!bloom_guard->ready || !bloom_guard->sampled_layout) {
                destroy_frame_bloom_targets(slot);
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Bloom pipeline resources are not ready for persistent target binding.");
            }
            image_binding = bloom_guard->image_binding;
            sampler_binding = bloom_guard->sampler_binding;
            sampler = bloom_guard->sampler;
            sampled_layout = bloom_guard->sampled_layout;
        }
        auto create_group = [&](RHI::TextureViewHandle source_view) -> Core::RendererExpected<RHI::BindGroupHandle> {
            const array<RHI::BindGroupEntry, 2> entries{
                RHI::BindGroupEntry{.binding = image_binding, .texture_view = source_view},
                RHI::BindGroupEntry{.binding = sampler_binding, .sampler = sampler},
            };
            auto group = device->create_bind_group(RHI::BindGroupDesc{
                .layout = sampled_layout,
                .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                .label = "persistent bloom source bind group",
            });
            if (!group) return unexpected(graphics_error_from_rhi(group.error(), "create persistent bloom bind group"));
            return *group;
        };

        slot.bloom_targets.downsample_bind_groups.resize(slot.bloom_targets.views.size());
        slot.bloom_targets.upsample_bind_groups.resize(slot.bloom_targets.views.size());
        // Level zero samples the semantic scene source, which may be a per-frame custom-graph transient;
        // its bind group is therefore always minted during recording instead of wasting a stale cached one.
        for (usize level = 1; level < slot.bloom_targets.views.size(); ++level) {
            auto group = create_group(slot.bloom_targets.views[level - 1]);
            if (!group) { destroy_frame_bloom_targets(slot); return unexpected(group.error()); }
            slot.bloom_targets.downsample_bind_groups[level] = *group;
        }
        for (usize level = 1; level < slot.bloom_targets.views.size(); ++level) {
            auto group = create_group(slot.bloom_targets.views[level]);
            if (!group) { destroy_frame_bloom_targets(slot); return unexpected(group.error()); }
            slot.bloom_targets.upsample_bind_groups[level] = *group;
        }
        return {};
    }

    void Renderer::destroy_frame_bloom_targets(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_bloom_targets");
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
            for (RHI::TextureHandle texture : slot.bloom_targets.textures) {
                if (texture) device->destroy_texture(texture);
            }
        }
        slot.bloom_targets = {};
    }

    Core::RendererResult Renderer::ensure_frame_composite_target(FrameInFlight &slot,
                                                                  Core::Extent2D extent,
                                                                  RHI::Format format) {
        ZoneScopedN("Renderer::ensure_frame_composite_target");
        const bool matches = slot.composite_target.extent.x == extent.x &&
            slot.composite_target.extent.y == extent.y &&
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
            .extent = RHI::Extent3D{.width = extent.x, .height = extent.y, .depth_or_layers = 1},
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::TransferSrc,
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
        ZoneScopedN("Renderer::destroy_frame_composite_target");
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
        ZoneScopedN("Renderer::ensure_frame_gpu_timing_target");
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
        ZoneScopedN("Renderer::destroy_frame_gpu_timing_target");
        if (RHI::RhiDevice *device = rhi_device(); device != nullptr && slot.gpu_timing.query_set) {
            device->destroy_query_set(slot.gpu_timing.query_set);
        }
        slot.gpu_timing = {};
    }

    Core::RendererResult Renderer::ensure_frame_pregraph_gpu_timing_target(FrameInFlight &slot) {
        ZoneScopedN("Renderer::ensure_frame_pregraph_gpu_timing_target");
        if (slot.pregraph_gpu_timing_query_set) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }
        auto query_set = device->create_query_set(RHI::QuerySetDesc{
            .type = RHI::QueryType::Timestamp,
            .count = kPregraphGpuTimingQueryCount,
            .label = "renderer pre-graph gpu pass timing",
        });
        if (!query_set) {
            return unexpected(graphics_error_from_rhi(query_set.error(), "create pre-graph GPU pass timing query set"));
        }
        slot.pregraph_gpu_timing_query_set = *query_set;
        return {};
    }

    void Renderer::destroy_frame_pregraph_gpu_timing_target(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_pregraph_gpu_timing_target");
        if (RHI::RhiDevice *device = rhi_device(); device != nullptr && slot.pregraph_gpu_timing_query_set) {
            device->destroy_query_set(slot.pregraph_gpu_timing_query_set);
        }
        slot.pregraph_gpu_timing_query_set = {};
        slot.pregraph_gpu_timing_pending.clear();
    }

    void Renderer::reclaim_frame_slot(FrameInFlight &slot, bool destroy_retired_presentation) noexcept {
        ZoneScopedN("Renderer::reclaim_frame_slot");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (RHI::BindGroupHandle group : slot.transient_bind_groups) {
                if (group) {
                    device->destroy_bind_group(group);
                }
            }
            for (RHI::AccelerationStructureHandle acceleration_structure : slot.transient_acceleration_structures) {
                if (acceleration_structure) {
                    device->destroy_acceleration_structure(acceleration_structure);
                }
            }
            for (RHI::BufferHandle buffer : slot.transient_buffers) {
                if (buffer) {
                    device->destroy_buffer(buffer);
                }
            }
            for (RHI::RenderBundleHandle bundle : slot.transient_render_bundles) {
                if (bundle) {
                    device->destroy_render_bundle(bundle);
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
            for (RHI::CommandBufferHandle command_buffer : slot.command_buffers) {
                if (command_buffer) {
                    device->destroy_command_buffer(command_buffer);
                }
            }
        }
        slot.transient_bind_groups.clear();
        slot.transient_acceleration_structures.clear();
        slot.transient_buffers.clear();
        slot.transient_render_bundles.clear();
        slot.transient_texture_views.clear();
        slot.transient_textures.clear();
        if (destroy_retired_presentation) {
            slot.retired_presentation_texture_views.clear();
            slot.retired_presentation_textures.clear();
            slot.retired_swapchains.clear();
        }
        slot.command_buffers.clear();
        slot.spectral_tlas = {};
        slot.spectral_scene_instances = {};
        slot.spectral_materials = {};
        slot.spectral_material_textures.clear();
        slot.spectral_frame_constants = {};
        slot.spectral_photon_constants = {};
        slot.spectral_scene_bounds = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
    }

    void Renderer::drain_frames_in_flight(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::drain_frames_in_flight");
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

    void Renderer::reclaim_completed_presentation_fences(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::reclaim_completed_presentation_fences");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !device->is_enabled(RHI::Feature::SwapchainMaintenance)) {
            return;
        }
        for (auto it = record.active_presentation_completion_fences.begin();
             it != record.active_presentation_completion_fences.end();) {
            const RHI::FenceHandle fence = *it;
            const auto waited = device->wait_fences(span<const RHI::FenceHandle>{&fence, 1}, true, 0);
            if (!waited) {
                Foundation::log_warn("Could not poll presentation-completion fence: {}", waited.error().message);
                ++it;
            } else if (*waited) {
                device->destroy_fence(fence);
                it = record.active_presentation_completion_fences.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Renderer::reclaim_completed_retired_presentations(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::reclaim_completed_retired_presentations");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !device->is_enabled(RHI::Feature::SwapchainMaintenance)) {
            return;
        }
        for (auto it = record.retired_presentation_resources.begin();
             it != record.retired_presentation_resources.end();) {
            bool complete = true;
            for (const RHI::FenceHandle fence : it->completion_fences) {
                const auto waited = device->wait_fences(span<const RHI::FenceHandle>{&fence, 1}, true, 0);
                if (!waited) {
                    Foundation::log_warn("Could not poll retired presentation fence: {}", waited.error().message);
                    complete = false;
                    break;
                }
                if (!*waited) {
                    complete = false;
                    break;
                }
            }
            if (!complete) {
                ++it;
                continue;
            }

            for (const RHI::FenceHandle fence : it->completion_fences) {
                device->destroy_fence(fence);
            }
            if (it->depth_view) {
                device->destroy_texture_view(it->depth_view);
            }
            if (it->depth_texture) {
                device->destroy_texture(it->depth_texture);
            }
            if (it->swapchain) {
                device->destroy_swapchain(it->swapchain);
            }
            it = record.retired_presentation_resources.erase(it);
        }
    }

    void Renderer::destroy_retired_presentations(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::destroy_retired_presentations");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (RetiredPresentationResources &retired : record.retired_presentation_resources) {
                for (const RHI::FenceHandle fence : retired.completion_fences) {
                    device->destroy_fence(fence);
                }
                if (retired.depth_view) {
                    device->destroy_texture_view(retired.depth_view);
                }
                if (retired.depth_texture) {
                    device->destroy_texture(retired.depth_texture);
                }
                if (retired.swapchain) {
                    device->destroy_swapchain(retired.swapchain);
                }
            }
        }
        record.retired_presentation_resources.clear();
    }

    void Renderer::maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept {
        ZoneScopedN("Renderer::maybe_flush_retired_swapchains");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr && device->is_enabled(RHI::Feature::SwapchainMaintenance)) {
            // Every retired generation has exact presentation-completion fences, so polling is enough:
            // do not ever idle unrelated windows just because this one is being resized.
            reclaim_completed_retired_presentations(record);
            return;
        }

        usize retired_count = 0;
        for (const FrameInFlight &slot : record.frames_in_flight) {
            retired_count += slot.retired_swapchains.size();
        }
        // Portable fallback: without presentation completion tracking, the only proof that an old
        // swapchain is no longer referenced by vkQueuePresentKHR is a device-wide idle point.
        const usize threshold = opportunistic ? 1 : retired_swapchain_flush_threshold;
        if (retired_count < threshold) {
            return;
        }
        ScopedRendererStageTimer timer{"flush retired swapchains"};
        drain_frames_in_flight(record);
    }

    void Renderer::destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::destroy_rhi_presentation_resources");
        if (RHI::RhiDevice *device = rhi_device()) {
            // Must happen before drain_frames_in_flight()'s wait_idle() / anything that destroys this
            // record's swapchain -- an outstanding presentation-coordinator job that hasn't actually
            // issued its vkQueuePresentKHR call yet is invisible to wait_idle() (it only knows about
            // work already handed to the driver). Result/error ignored: this is best-effort teardown,
            // and the window is going away regardless. See WindowSurfaceRecord::pending_present's doc
            // comment.
            (void)drain_pending_present(record, nullptr);
            // Per-window teardown is allowed to stall. The window is about to disappear, so first make
            // every submitted frame for this surface complete and reclaim frame-owned command buffers,
            // transient targets, and retired swapchains. Without this, Wayland WSI objects backing
            // presented swapchain images can still be attached when SDL destroys the wl_surface, producing
            // "mesa vk display queue ... destroyed while proxies still attached" warnings.
            drain_frames_in_flight(record);
            // wait_idle() above also completes every presentation fence, so teardown can reclaim the
            // maintenance path's old generations and any current-generation fences unconditionally.
            destroy_retired_presentations(record);
            for (const RHI::FenceHandle fence : record.active_presentation_completion_fences) {
                device->destroy_fence(fence);
            }
            record.active_presentation_completion_fences.clear();
            record.pending_present_completion_fence.reset();
            for (FrameInFlight &slot : record.frames_in_flight) {
                reclaim_frame_slot(slot, true);
                destroy_text_frame_resources(*device, slot.text_overlay_resources);
                destroy_frame_bloom_targets(slot);
                destroy_frame_composite_target(slot);
                destroy_frame_shadow_targets(slot);
                destroy_frame_atmosphere_targets(slot);
                destroy_frame_gpu_timing_target(slot);
                destroy_frame_pregraph_gpu_timing_target(slot);
                destroy_frame_spectral_photon_targets(slot);
                destroy_frame_deferred_targets(slot);
                if (slot.fence) {
                    device->destroy_fence(slot.fence);
                    slot.fence = {};
                }
                slot.submitted = false;
            }
            record.frames_in_flight.clear();
            destroy_spectral_accumulation_target(record);
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
        ZoneScopedN("Renderer::wait_idle");
        if (graphics_backend_) {
            graphics_backend_->wait_idle();
        }
    }

    const RHI::FeatureNegotiationReport *Renderer::feature_negotiation_report() const noexcept {
        ZoneScopedN("Renderer::feature_negotiation_report");
        const RHI::RhiDevice *device = rhi_device();
        return device != nullptr ? &device->feature_negotiation_report() : nullptr;
    }

    optional<Core::GpuInfo> Renderer::gpu_info() const {
        ZoneScopedN("Renderer::gpu_info");
        if (!graphics_backend_) {
            return std::nullopt;
        }
        return graphics_backend_->gpu_info();
    }

    RHI::RhiDevice *Renderer::rhi_device() noexcept {
        ZoneScopedN("Renderer::rhi_device");
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

    const RHI::RhiDevice *Renderer::rhi_device() const noexcept {
        ZoneScopedN("Renderer::rhi_device");
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

} // namespace SFT::Renderer
