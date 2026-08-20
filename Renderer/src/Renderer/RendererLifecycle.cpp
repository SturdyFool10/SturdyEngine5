#include <Foundation/Foundation.hpp>

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
#include <WindowManager/WindowManager.hpp>

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


        constexpr usize retired_swapchain_flush_threshold = 6;


        constexpr u32 kPregraphGpuTimingQueryCount = 4u;

        /// Resolves the front-face convention after applying a model transform.
        ///
        /// @param authored_front_face Front-face winding authored for the untransformed mesh.
        /// @param world_transform Transform applied to the mesh.
        ///
        /// @return Returns the winding that remains front-facing after the transform.
        /// @note A transform with a negative determinant mirrors geometry and therefore reverses winding.
        [[nodiscard]] RHI::FrontFace transformed_front_face(
            RHI::FrontFace authored_front_face, const glm::mat4 &world_transform) noexcept {
            const glm::vec3 x{world_transform[0]};
            const glm::vec3 y{world_transform[1]};
            const glm::vec3 z{world_transform[2]};
            const bool reverses_winding = glm::dot(glm::cross(x, y), z) < 0.0f;
            if (!reverses_winding) {
                return authored_front_face;
            }
            return authored_front_face == RHI::FrontFace::CounterClockwise
                       ? RHI::FrontFace::Clockwise
                       : RHI::FrontFace::CounterClockwise;
        }

        class ScopedRendererStageTimer {
          public:
            /// Constructs a `ScopedRendererStageTimer` from the supplied initialization values.
            ///
            /// @param stage `stage` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            explicit ScopedRendererStageTimer(const char *stage) noexcept
                : stage_(stage), start_(steady_clock::now()) {}


            /// Constructs a `ScopedRendererStageTimer` from the supplied initialization values.
            ///
            /// @param stage `stage` value used by the operation.
            /// @param accumulate_into `accumulate_into` value used by the operation.
            ///
            /// @note This function does not throw exceptions.
            ScopedRendererStageTimer(const char *stage, vector<pair<string, f64>> *accumulate_into) noexcept
                : stage_(stage), start_(steady_clock::now()), accumulate_into_(accumulate_into) {}

            /// Destroys the `ScopedRendererStageTimer` and releases resources owned by it.
            ///
            /// @note This function does not throw exceptions.
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


        /// Renders graph pass timing category using the current rendering state.
        ///
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Performs the snapshot timings operation for `Renderer` using the supplied arguments.
        ///
        /// @param timings `timings` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<pair<string, f64>> snapshot_timings(const vector<pair<UString, f64>> &timings) {
            vector<pair<string, f64>> snapshot;
            snapshot.reserve(timings.size());
            for (const auto &[label, milliseconds] : timings) {
                snapshot.emplace_back(label.cpp_string(), milliseconds);
            }
            return snapshot;
        }

        /// Performs the framebuffer extent operation for `Renderer` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::Extent2D framebuffer_extent(WindowManager::Window &window) {
            if (auto size = window.framebuffer_size()) {
                return *size;
            }
            return {};
        }


        /// Performs the HDR presentation format operation for `Renderer` using the supplied arguments.
        ///
        /// @param presentation `presentation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::Format hdr_presentation_format(const Core::PresentationSettings &presentation) noexcept {
            if (!static_cast<bool>(presentation.hdr_enabled)) {
                return RHI::Format::BGRA8UnormSrgb;
            }
            switch (presentation.hdr_color_space) {

                case Core::HdrColorSpaceMode::ScrgbLinear: return RHI::Format::RGBA16Float;


                case Core::HdrColorSpaceMode::Hdr10St2084:
                case Core::HdrColorSpaceMode::Hdr10Hlg:
                case Core::HdrColorSpaceMode::DolbyVision:
                default: return RHI::Format::RGB10A2Unorm;
            }
        }

        /// Performs the HDR presentation color space operation for `Renderer` using the supplied arguments.
        ///
        /// @param presentation `presentation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Returns a human-readable name for the supplied HDR color space value.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *hdr_color_space_name(Core::HdrColorSpaceMode mode) noexcept {
            switch (mode) {
                case Core::HdrColorSpaceMode::Hdr10St2084: return "HDR10 (ST2084/PQ)";
                case Core::HdrColorSpaceMode::ScrgbLinear: return "scRGB linear";
                case Core::HdrColorSpaceMode::Hdr10Hlg: return "HLG";
                case Core::HdrColorSpaceMode::DolbyVision: return "Dolby Vision (best-effort)";
            }
            return "unknown";
        }

        /// Performs the graphics error from shader operation for `Renderer` using the supplied arguments.
        ///
        /// @param error Error value describing the failure.
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Constructs a `Renderer` in its default state.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Renderer::Renderer() = default;

    /// Destroys the `Renderer` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    Renderer::~Renderer() {
        ZoneScopedN("Renderer::~Renderer");
        wait_idle();
        destroy_all_resources();
    }

    /// Initializes the `Renderer` for use.
    ///
    /// @param create_info Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `GraphicsBackendErrorCode::InitializationFailed`.
    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::initialize(
        const Core::RendererCreateInfo &create_info) {
        ZoneScopedN("Renderer::initialize");
        if (initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Renderer is already initialized."});
        }
        if (!graphics_backend_) {
            graphics_backend_ = Core::create_engine_backend(create_info.backend);
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


    /// Submits draw.
    ///
    /// @param mesh_handle Handle identifying the target object or resource.
    /// @param material_handle Handle identifying the target object or resource.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::submit_draw(MeshHandle mesh_handle, MaterialInstanceHandle material_handle) {
        ZoneScopedN("Renderer::submit_draw");
        const MeshResource *mesh_resource = mesh(mesh_handle);
        if (mesh_resource == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot submit a draw for an unknown mesh.");
        }
        if (material_instance(material_handle) == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot submit a draw for an unknown material instance.");
        }
        frame_draws_.push_back(RenderItem{
            .mesh = mesh_handle,
            .material = material_handle,
            .world_bounds_center = mesh_resource->bounds_center,
            .world_bounds_radius = mesh_resource->bounds_radius,
        });
        return {};
    }

    /// Renders frame using the current rendering state.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::render_frame(const RenderFrameDesc &desc) {
        ZoneScopedN("Renderer::render_frame");


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
                const MeshResource *mesh_resource = mesh(renderable.mesh);
                if (mesh_resource == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Scene renderable references an unknown mesh.");
                }
                if (material_instance(renderable.material) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Scene renderable references an unknown material instance.");
                }
                const f32 scale_x = glm::length(glm::vec3{renderable.world_transform[0]});
                const f32 scale_y = glm::length(glm::vec3{renderable.world_transform[1]});
                const f32 scale_z = glm::length(glm::vec3{renderable.world_transform[2]});
                const f32 maximum_scale = std::max({scale_x, scale_y, scale_z});
                const glm::vec3 world_bounds_center =
                    glm::vec3{renderable.world_transform * glm::vec4{mesh_resource->bounds_center, 1.0f}};
                submission.draws.push_back(RenderItem{
                    .mesh = renderable.mesh,
                    .material = renderable.material,
                    .world_transform = renderable.world_transform,
                    .stable_id = renderable.stable_id,
                    .sort_key = renderable.sort_key,
                    .casts_shadows = renderable.casts_shadows,
                    .cull_mode = renderable.cull_mode,
                    .front_face = transformed_front_face(renderable.front_face, renderable.world_transform),
                    .world_bounds_center = world_bounds_center,
                    .world_bounds_radius = mesh_resource->bounds_radius * maximum_scale,
                });
            }


            submission.gizmo_draws.reserve(desc.view.gizmo_renderables.size());
            for (const SceneRenderable &renderable : desc.view.gizmo_renderables) {
                const MeshResource *mesh_resource = mesh(renderable.mesh);
                if (mesh_resource == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Gizmo renderable references an unknown mesh.");
                }
                if (material_instance(renderable.material) == nullptr) {
                    return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Gizmo renderable references an unknown material instance.");
                }
                const f32 scale_x = glm::length(glm::vec3{renderable.world_transform[0]});
                const f32 scale_y = glm::length(glm::vec3{renderable.world_transform[1]});
                const f32 scale_z = glm::length(glm::vec3{renderable.world_transform[2]});
                const f32 maximum_scale = std::max({scale_x, scale_y, scale_z});
                const glm::vec3 world_bounds_center =
                    glm::vec3{renderable.world_transform * glm::vec4{mesh_resource->bounds_center, 1.0f}};
                submission.gizmo_draws.push_back(RenderItem{
                    .mesh = renderable.mesh,
                    .material = renderable.material,
                    .world_transform = renderable.world_transform,
                    .stable_id = renderable.stable_id,
                    .sort_key = renderable.sort_key,
                    .casts_shadows = renderable.casts_shadows,
                    .cull_mode = renderable.cull_mode,
                    .front_face = transformed_front_face(renderable.front_face, renderable.world_transform),
                    .world_bounds_center = world_bounds_center,
                    .world_bounds_radius = mesh_resource->bounds_radius * maximum_scale,
                });
            }
        }

        return render_frame_dispatch(desc.surface, desc.frame, submission);
    }

    /// Renders frame using the current rendering state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::render_frame(Core::RenderSurfaceHandle surface,
                                                const Core::FrameInput &frame) {
        ZoneScopedN("Renderer::render_frame");


        poll_shader_hot_reload();


        FrameSubmission submission{};
        submission.draws = std::move(frame_draws_);
        frame_draws_.clear();

        return render_frame_dispatch(surface, frame, submission);
    }

    /// Renders frame dispatch using the current rendering state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    /// @param submission `submission` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `GraphicsBackendErrorCode::DeviceLost`.
    Core::RendererResult Renderer::render_frame_dispatch(Core::RenderSurfaceHandle surface,
                                                          const Core::FrameInput &frame,
                                                          FrameSubmission &submission) {
        ZoneScopedN("Renderer::render_frame_dispatch");
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }


        {
            ScopedRendererStageTimer timer{"sort render items",
                                           submission.render_graph.debug_overlay ? &submission.pre_dispatch_stage_timings_ms : nullptr};
            std::sort(submission.draws.begin(), submission.draws.end(), [](const RenderItem &a, const RenderItem &b) {
                if (!(a.material == b.material)) {
                    return a.material.value < b.material.value;
                }
                if (!(a.mesh == b.mesh)) {
                    return a.mesh.value < b.mesh.value;
                }
                if (a.cull_mode != b.cull_mode) {
                    return static_cast<u32>(a.cull_mode) < static_cast<u32>(b.cull_mode);
                }
                return static_cast<u32>(a.front_face) < static_cast<u32>(b.front_face);
            });


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


    /// Finds or creates the RHI presentation resources required by the operation.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Renders item visible using the current rendering state.
    ///
    /// @param item `item` value used by the operation.
    /// @param frustum `frustum` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool Renderer::render_item_visible(const RenderItem &item, const Frustum &frustum) noexcept {
        ZoneScopedN("Renderer::render_item_visible");
        return frustum_intersects_sphere(frustum, item.world_bounds_center, item.world_bounds_radius);
    }

    /// Records render item using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param item `item` value used by the operation.
    /// @param color_formats Format used for the resource, render target, or conversion.
    /// @param depth_format Format used for the resource, render target, or conversion.
    /// @param frame_index Zero-based index of the target element or entry.
    /// @param view_projection `view_projection` value used by the operation.
    /// @param depth_only `depth_only` value used by the operation.
    /// @param binding_state `binding_state` value used by the operation.
    /// @param standard_depth_test `standard_depth_test` value used by the operation.
    /// @param shadow_map `shadow_map` value used by the operation.
    /// @param shadow_depth_bias `shadow_depth_bias` value used by the operation.
    /// @param shadow_slope_bias `shadow_slope_bias` value used by the operation.
    /// @param samples `samples` value used by the operation.
    /// @param with_object_history `with_object_history` value used by the operation.
    /// @param object_history_group `object_history_group` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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
                                                      shadow_depth_bias, shadow_slope_bias, item.cull_mode,
                                                      item.front_face, samples)
                            : (use_object_history
                                   ? history_pipeline_for(*material_template_resource, color_formats, depth_format,
                                                          standard_depth_test, item.cull_mode, item.front_face, samples)
                                   : material_pipeline_for(*material_template_resource, color_formats, depth_format,
                                                           standard_depth_test, item.cull_mode, item.front_face, samples));
        if (!pipeline) {
            return unexpected(pipeline.error());
        }


        if (!(binding_state.pipeline == *pipeline)) {
            pass.set_pipeline(*pipeline);
            binding_state.pipeline = *pipeline;
        }

        if (use_object_history) {


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


        constexpr usize kParallelRecordThreshold = 128;
    } // namespace

    /// Records render items culled using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param items `items` value used by the operation.
    /// @param frustum `frustum` value used by the operation.
    /// @param color_formats Format used for the resource, render target, or conversion.
    /// @param depth_format Format used for the resource, render target, or conversion.
    /// @param frame_index Zero-based index of the target element or entry.
    /// @param view_projection `view_projection` value used by the operation.
    /// @param depth_only `depth_only` value used by the operation.
    /// @param standard_depth_test `standard_depth_test` value used by the operation.
    /// @param bundle_label `bundle_label` value used by the operation.
    /// @param use_bundles `use_bundles` value used by the operation.
    /// @param retired_bundles `retired_bundles` value used by the operation.
    /// @param shadow_map `shadow_map` value used by the operation.
    /// @param shadow_depth_bias `shadow_depth_bias` value used by the operation.
    /// @param shadow_slope_bias `shadow_slope_bias` value used by the operation.
    /// @param samples `samples` value used by the operation.
    /// @param with_object_history `with_object_history` value used by the operation.
    /// @param object_history_group `object_history_group` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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


        retired_bundles.insert(retired_bundles.end(), bundles.begin(), bundles.end());
        if (has_error) {
            return first_error;
        }
        return {};
    }

    /// Records shadow view chunk using the supplied arguments and current state.
    ///
    /// @param encoder `encoder` value used by the operation.
    /// @param views `views` value used by the operation.
    /// @param draws Draw descriptions processed in submission order.
    /// @param depth_format Format used for the resource, render target, or conversion.
    /// @param frame_index Zero-based index of the target element or entry.
    /// @param shadow_depth_bias `shadow_depth_bias` value used by the operation.
    /// @param shadow_slope_bias `shadow_slope_bias` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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
                if (!item.casts_shadows || !render_item_visible(item, view.frustum)) {
                    continue;
                }
                if (Core::RendererResult recorded = record_render_item(
                        encoder, item, span<const RHI::Format>{}, depth_format, frame_index,
                        view.view_projection,                true, binding_state,
                                                false,                true, shadow_depth_bias,
                        shadow_slope_bias, RHI::SampleCount::X1, false, RHI::BindGroupHandle{});
                    !recorded.has_value()) {
                    return recorded;
                }
            }
        }
        return {};
    }

    /// Finds or creates the RHI depth resources required by the operation.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Drains pending present using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    /// @param stage_timings_ms `stage_timings_ms` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::SurfaceLost`, `RhiErrorCode::FullScreenExclusiveLost`.
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


            if (completion_fence) {
                std::erase(record.active_presentation_completion_fences, *completion_fence);
                if (RHI::RhiDevice *device = rhi_device()) {
                    device->destroy_fence(*completion_fence);
                }
            }


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

    /// Recreates RHI swapchain using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    /// @param frame_index Zero-based index of the target element or entry.
    /// @param known_extent `known_extent` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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
        const bool explicit_presentation_change = record.explicit_presentation_change_pending;


        const RHI::PresentationResolution old_presentation =
            old_swapchain ? device->presentation_resolution(old_swapchain) : RHI::PresentationResolution{};
        const bool old_swapchain_supports_completion_fence = old_presentation.supports_completion_fence;
        bool old_presentation_destroyed_before_create = false;

        const auto destroy_old_presentation_resources = [&]() noexcept {
            for (const RHI::FenceHandle fence : record.active_presentation_completion_fences) {
                device->destroy_fence(fence);
            }
            record.active_presentation_completion_fences.clear();
            if (old_depth_view) {
                device->destroy_texture_view(old_depth_view);
            }
            if (old_depth_texture) {
                device->destroy_texture(old_depth_texture);
            }
            if (old_swapchain) {
                device->destroy_swapchain(old_swapchain);
            }
        };

        if (explicit_presentation_change && (old_swapchain || old_depth_texture || old_depth_view)) {
            drain_frames_in_flight(record);
            if (old_swapchain_supports_completion_fence) {
                const auto wait_completion_fences = [&](span<const RHI::FenceHandle> fences) -> Core::RendererResult {
                    if (fences.empty()) {
                        return {};
                    }
                    const auto waited = device->wait_fences(fences, true);
                    if (!waited) {
                        return unexpected(graphics_error_from_rhi(
                            waited.error(), "wait for presentation completion before swapchain mode change"));
                    }
                    if (!*waited) {
                        return Core::graphics_backend_error(
                            Core::GraphicsBackendErrorCode::OperationFailed,
                            "Presentation completion wait did not finish before swapchain mode change.");
                    }
                    return {};
                };
                for (const RetiredPresentationResources &retired : record.retired_presentation_resources) {
                    if (Core::RendererResult waited = wait_completion_fences(retired.completion_fences); !waited) {
                        return waited;
                    }
                }
                if (Core::RendererResult waited = wait_completion_fences(record.active_presentation_completion_fences);
                    !waited) {
                    return waited;
                }
            }
            destroy_retired_presentations(record);
            destroy_old_presentation_resources();
            record.rhi_swapchain = {};
            record.depth_texture = {};
            record.depth_view = {};
            old_presentation_destroyed_before_create = true;
        }

        RHI::SwapchainDesc swapchain_desc{
            .surface = record.rhi_surface,
            .width = extent.x,
            .height = extent.y,
            .format = hdr_presentation_format(record.presentation),
            .color_space = hdr_presentation_color_space(record.presentation),


            .present_strategy = Core::resolve_present_strategy(record.presentation),
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::TransferDst,
            .composite_alpha = static_cast<bool>(record.presentation.transparent_composition)
                                   ? RHI::CompositeAlphaMode::Premultiplied
                                   : RHI::CompositeAlphaMode::Opaque,
            .clipped = true,
            .image_count = record.presentation.swapchain_image_count != 0
                               ? record.presentation.swapchain_image_count
                               : record.desired_frames_in_flight + 1,


            .frames_in_flight = capabilities_.max_frames_in_flight,
            .old_swapchain = old_presentation_destroyed_before_create ? RHI::SwapchainHandle{} : old_swapchain,
            .allow_present_from_compute = static_cast<bool>(record.presentation.allow_present_from_compute),


            .request_full_screen_exclusive =
                record.window != nullptr &&
                record.window->fullscreen_mode() == WindowManager::WindowMode::ExclusiveFullscreen,
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
        record.explicit_presentation_change_pending = false;


        const RHI::PresentationResolution new_presentation = device->presentation_resolution(record.rhi_swapchain);
        const bool must_retire_native_before_composition = old_swapchain &&
            !old_presentation.via_composition_present && new_presentation.via_composition_present;


        const bool can_retire_composition_immediately = old_swapchain &&
            old_presentation.via_composition_present && new_presentation.via_composition_present;
        if (!old_presentation_destroyed_before_create &&
            (old_swapchain || old_depth_texture || old_depth_view)) {
            if (must_retire_native_before_composition) {
                device->wait_idle();
                destroy_old_presentation_resources();
            } else if (can_retire_composition_immediately) {
                destroy_old_presentation_resources();
            } else if (old_swapchain_supports_completion_fence) {


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

                    destroy_old_presentation_resources();
                }
            }
        }
        return ensure_rhi_depth_resources(record);
    }

    /// Renders frame RHI using the current rendering state.
    ///
    /// @param record `record` value used by the operation.
    /// @param frame `frame` value used by the operation.
    /// @param submission `submission` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `RhiErrorCode::NotReady`, `RhiErrorCode::SurfaceLost`.
    Core::RendererResult Renderer::render_frame_rhi(WindowSurfaceRecord &record,
                                                    const Core::FrameInput &frame,
                                                    FrameSubmission &submission) {
        ZoneScopedN("Renderer::render_frame_rhi");
        auto backend_operation = backend_operation_mutex_.lock();
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


            reclaim_frame_slot(slot, false);
        }


        vector<pair<string, f64>> current_frame_cpu_stage_timings_ms;


        if (slot.submitted) {
            {
                ScopedRendererStageTimer timer{"wait in-flight frame fence", &current_frame_cpu_stage_timings_ms};
                auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true);
                if (!waited) {
                    return unexpected(graphics_error_from_rhi(waited.error(), "wait in-flight frame fence"));
                }
                if (!*waited) {


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


        vector<pair<UString, f64>> gpu_pass_timings_ms;
        if (slot.gpu_timing.has_pending_results) {
            const f32 period_ns = device->limits().timestamp_period_ns;
            unordered_map<UString, f64> totals_ms;
            bool any_read = false;


            const auto accumulate_query_set = [&](RHI::QuerySetHandle query_set,
                                                   const vector<RenderGraph::GpuPassTiming> &timings) {
                if (!query_set || timings.empty()) {
                    return;
                }


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


                auto published_timings = record.last_frame_timings->lock();
                published_timings->gpu_pass_timings_ms = snapshot_timings(gpu_pass_timings_ms);
                published_timings->has_data = true;
            }
            slot.gpu_timing.has_pending_results = false;
        }


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


            if (frame.live_resize && record.pending_present && !record.pending_present->is_done()) {
                return {};
            }
            if (Core::RendererResult drained = drain_pending_present(record, &current_frame_cpu_stage_timings_ms);
                !drained.has_value()) {
                return drained;
            }
            reclaim_completed_presentation_fences(record);


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

        const RHI::PresentationResolution active_presentation =
            !offscreen_output && record.rhi_swapchain
                ? device->presentation_resolution(record.rhi_swapchain)
                : RHI::PresentationResolution{};
        const bool hdr_output = !offscreen_output &&
                                static_cast<bool>(record.presentation.hdr_enabled) &&
                                active_presentation.effective_color_space != RHI::ColorSpace::SrgbNonlinear;
        const RHI::Format output_format = offscreen_output
            ? RHI::Format::BGRA8UnormSrgb
            : active_presentation.effective_format;
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


        const vector<InstancedBatch> instanced_batches =
            submission.render_graph.render_scene ? detect_instanced_batches(submission.draws) : vector<InstancedBatch>{};


        const u32 scene_frame_count = capabilities_.max_frames_in_flight;
        SceneFrameGpuResources &instance_cull_resources = record.scene_frame_resources[frame.frame_index % scene_frame_count];


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
        const bool direct_overlay_presentation =
            !submission.render_graph.render_scene && !submission.render_graph.tone_mapping &&
            !submission.render_graph.bloom && submission.render_graph.post_process_aa == 0u &&
            submission.render_graph.custom_post_processes.empty() &&
            submission.render_graph.custom_graph.passes.empty() && submission.gizmo_draws.empty() &&
            static_cast<bool>(submission.render_graph.ui_overlay) && !submission.render_graph.draw_overlay_text;
        const bool direct_overlay_display_transform =
            direct_overlay_presentation &&
            (hdr_output || static_cast<bool>(record.presentation.transparent_composition));
        f32 ui_reference_white_nits = submission.render_graph.tone_mapping_hdr_paper_white_nits;
        bool platform_reference_white = false;
        if (hdr_output && record.window != nullptr) {
            if (const optional<WindowManager::WindowHdrProperties> properties =
                    record.window->hdr_properties();
                properties && properties->hdr_enabled && properties->sdr_white_level > 0.0f) {


                ui_reference_white_nits = properties->sdr_white_level * 80.0f;
                platform_reference_white = true;
            }
        }
        if (hdr_output) {
            ui_reference_white_nits *= std::clamp(
                submission.render_graph.ui_overlay.hdr_reference_white_scale, 0.25f, 4.0f);
        }
        if (hdr_output &&
            (record.ui_reference_white_nits == 0.0f ||
             std::abs(record.ui_reference_white_nits - ui_reference_white_nits) >= 0.5f)) {
            Foundation::log_info(
                "UI HDR reference white: {:.1f} nits ({}).",
                ui_reference_white_nits,
                platform_reference_white ? "window compositor SDR white" : "render-graph fallback");
            record.ui_reference_white_nits = ui_reference_white_nits;
        }
        if (!direct_overlay_presentation || direct_overlay_display_transform) {
            if (Core::RendererResult tonemap_ready = ensure_tonemap_resources(); !tonemap_ready.has_value()) {
                return tonemap_ready;
            }
            if (auto tonemap_pipeline = tonemap_pipeline_for(output_format); !tonemap_pipeline) {
                return unexpected(tonemap_pipeline.error());
            }
        }


        struct AcquiredImageGuard {
            WindowSurfaceRecord *record = nullptr;
            bool resolved = false;
            ~AcquiredImageGuard() noexcept {
                if (record != nullptr && !resolved) {
                    record->rhi_swapchain_dirty = true;
                }
            }
        } acquired_image_guard;


        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer frame"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create RHI command encoder"));
        }


        const bool gpu_timing_enabled = submission.render_graph.debug_overlay;
        if (gpu_timing_enabled) {
            if (Core::RendererResult pregraph_timing = ensure_frame_pregraph_gpu_timing_target(slot);
                !pregraph_timing.has_value()) {
                return pregraph_timing;
            }
            slot.pregraph_gpu_timing_pending.clear();
            (**encoder).reset_query_set(slot.pregraph_gpu_timing_query_set, 0, kPregraphGpuTimingQueryCount);
        }
        const bool spectral_scene_active = submission.render_graph.spectral_path_tracing.mode !=
                                               SpectralRenderMode::RasterDeferred ||
                                           submission.render_graph.surfel_gi.enabled;
        if (spectral_scene_active) {
            if (gpu_timing_enabled) {
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
        }


        const bool spectral_photon_mapping = full_path_tracing &&
            submission.render_graph.spectral_path_tracing.photon_count > 0u;
        const bool spectral_photon_emission_needed = spectral_photon_mapping &&
            (!slot.spectral_photon_targets.populated ||
             slot.spectral_photon_targets.state_signature != spectral_photon_signature);


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


            const f32 overlay_fps = frame.delta_seconds > 0.0 ? static_cast<f32>(1.0 / frame.delta_seconds) : 0.0f;
            const optional<Core::GpuInfo> overlay_gpu_info = gpu_info();
            FrameTimingSnapshot overlay_timings{};
            {
                auto published_timings = record.last_frame_timings->lock();
                overlay_timings = *published_timings;
            }
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
                        if (offscreen_output) {
                            return string{"HDR: disabled (off-screen SDR)"};
                        }
                        if (static_cast<bool>(record.presentation.hdr_enabled) && active_presentation.degraded) {
                            return string{"HDR: requested; presentation degraded to SDR"};
                        }
                        return string{"HDR: disabled (SDR/sRGB)"};
                    }
                    return std::format("HDR: enabled ({}){}",
                                       hdr_color_space_name(record.presentation.hdr_color_space),
                                       active_presentation.degraded ? " (degraded)" : "");
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


            if (!overlay_timings.gpu_pass_timings_ms.empty()) {
                f64 gpu_total_ms = 0.0;
                for (const auto &[category, ms] : overlay_timings.gpu_pass_timings_ms) {
                    gpu_total_ms += ms;
                }
                overlay_lines.push_back(std::format("GPU total: {:.2f} ms", gpu_total_ms));
                for (const auto &[category, ms] : overlay_timings.gpu_pass_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", category, ms));
                }
            }


            if (!overlay_timings.cpu_stage_timings_ms.empty()) {
                f64 cpu_stage_total_ms = 0.0;
                for (const auto &[stage, ms] : overlay_timings.cpu_stage_timings_ms) {
                    cpu_stage_total_ms += ms;
                }
                overlay_lines.push_back(std::format("CPU frame total: {:.2f} ms", cpu_stage_total_ms));
                for (const auto &[stage, ms] : overlay_timings.cpu_stage_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", stage, ms));
                }
            }


            if (!overlay_timings.cpu_pass_timings_ms.empty()) {
                f64 cpu_pass_total_ms = 0.0;
                for (const auto &[category, ms] : overlay_timings.cpu_pass_timings_ms) {
                    cpu_pass_total_ms += ms;
                }
                overlay_lines.push_back(std::format("CPU pass recording total: {:.2f} ms", cpu_pass_total_ms));
                for (const auto &[category, ms] : overlay_timings.cpu_pass_timings_ms) {
                    overlay_lines.push_back(std::format("  {}: {:.2f} ms", category, ms));
                }
            }


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


        const bool output_uses_composition_present =
            !offscreen_output && acquired_surface->composition_present;


        if (spectral_photon_mapping) {
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
        }


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
                : output_uses_composition_present ? RHI::TextureLayout::General : RHI::TextureLayout::Present,
            .final_stage = offscreen_output
                ? RHI::PipelineStage::AllGraphics | RHI::PipelineStage::ComputeShader
                : output_uses_composition_present ? RHI::PipelineStage::AllCommands : RHI::PipelineStage::None,
            .final_access = offscreen_output
                ? RHI::AccessFlags::ShaderRead
                : output_uses_composition_present ? RHI::AccessFlags::MemoryRead : RHI::AccessFlags::None,
            .label = offscreen_output ? "off-screen final color"
                                      : output_uses_composition_present ? "composition color" : "swapchain color",
        });
        graph.mark_output(final_output);
        const RenderGraphTextureHandle ui_overlay_target = direct_overlay_display_transform
            ? graph.create_texture(RenderGraphTextureDesc{
                  .format = RHI::Format::RGBA16Float,
                  .extent = RHI::Extent3D{
                      .width = presentation_extent.x,
                      .height = presentation_extent.y,
                      .depth_or_layers = 1,
                  },
                  .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                  .label = "linear UI composition",
              })
            : final_output;
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

        graph.mark_output(hiz_pyramid_texture);


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


        const Frustum camera_frustum = frustum_from_view_projection(submission.view_projection);


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


                const bool shadow_atlas_uses_bundles =
                    device->is_enabled(RHI::Feature::RenderBundles) &&
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


                        submission.transient_render_bundles.insert(submission.transient_render_bundles.end(),
                                                                    bundles.begin(), bundles.end());
                        if (has_error) {
                            return first_error;
                        }
                        return {};
                    });
            }


            usize zprepass_visible_count = 0;
            for (const RenderItem &item : submission.draws) {
                if (render_item_visible(item, camera_frustum)) {
                    ++zprepass_visible_count;
                }
            }
            const bool zprepass_uses_bundles =
                device->is_enabled(RHI::Feature::RenderBundles) &&
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
                                                                      true,                         false, "z prepass",
                                                       zprepass_uses_bundles, submission.transient_render_bundles,
                                                                      false, 0.0f, 0.0f, framebuffer_samples);
                });


            usize gbuffer_visible_count = 0;
            for (const RenderItem &item : gbuffer_draws) {
                if (render_item_visible(item, camera_frustum)) {
                    ++gbuffer_visible_count;
                }
            }
            const bool gbuffer_uses_bundles =
                device->is_enabled(RHI::Feature::RenderBundles) &&
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
                            frame.frame_index, submission.view_projection,                false,
                                                    multisampled, "deferred gbuffer geometry",
                            gbuffer_uses_bundles, submission.transient_render_bundles,
                                           false, 0.0f, 0.0f, RHI::SampleCount::X1,
                                                    true, object_history_group);
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

        RenderGraphTextureHandle surfel_irradiance{};
        if (submission.render_graph.render_scene && !full_path_tracing) {
            auto surfel_gi_texture = build_surfel_gi_module(module_context, submission, slot, gbuffer_normal, depth_texture);
            if (!surfel_gi_texture.has_value()) {
                return unexpected(surfel_gi_texture.error());
            }
            surfel_irradiance = *surfel_gi_texture;
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
            lighting_pass.add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = surfel_irradiance});
            lighting_pass
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.x, .height = render_extent.y})
                .set_execute([this, &submission, &slot, render_extent, gbuffer_albedo, gbuffer_normal,
                              gbuffer_material, gbuffer_emissive, depth_texture, lighting_spectral_effect,
                              shadow_atlas, &shadow_frame, surfel_irradiance,
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
                        context.texture(surfel_irradiance).default_view,
                        slot.atmosphere_targets.constants_buffer,
                        submission.deferred_formats.scene_color,
                        submission.transient_bind_groups);
                });
        } else if (!submission.render_graph.render_scene && !direct_overlay_presentation) {

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
                                               false, binding_state,                         true);
                            !recorded.has_value()) {
                            return recorded;
                        }
                    }
                    return {};
                });
        }

        if (Core::RendererResult motion_blurred = build_motion_blur_module(
                module_context, submission, gbuffer_motion, depth_texture);
            !motion_blurred.has_value()) {
            return motion_blurred;
        }

        if (Core::RendererResult anti_aliased = build_post_process_aa_module(module_context, submission);
            !anti_aliased.has_value()) {
            return anti_aliased;
        }
        map_logical_texture(
            submission.render_graph.custom_graph.anti_aliasing_output,
            graph_resources.texture<RenderGraphSemantics::SceneHdrColor>());


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


        if (!direct_overlay_presentation) {
            if (Core::RendererResult tone_mapped = build_tonemap_module(
                    module_context, submission, output_format, hdr_output, record.presentation.hdr_color_space);
                !tone_mapped.has_value()) {
                return tone_mapped;
            }
        }

        if (submission.render_graph.debug_overlay && submission.render_graph.draw_overlay_text) {


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


            graph.add_render_pass("UI overlay"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = ui_overlay_target,
                    .load_op = direct_overlay_presentation ? RHI::LoadOp::Clear : RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                    .clear_color = static_cast<bool>(record.presentation.transparent_composition)
                                       ? RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f}
                                       : RHI::ClearColor{background.r, background.g, background.b, 1.0f},
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

        if (direct_overlay_display_transform) {
            RenderGraphSettings ui_display_settings{};
            ui_display_settings.tone_mapping = false;
            ui_display_settings.tone_mapping_exposure = 1.0f;
            ui_display_settings.tone_mapping_white_point = 1.0f;
            ui_display_settings.tone_mapping_saturation = 1.0f;
            ui_display_settings.tone_mapping_hdr_output = hdr_output;
            ui_display_settings.tone_mapping_hdr_color_space = record.presentation.hdr_color_space;
            ui_display_settings.tone_mapping_hdr_paper_white_nits = ui_reference_white_nits;
            ui_display_settings.tone_mapping_hdr_peak_nits = submission.render_graph.tone_mapping_hdr_peak_nits;

            graph.add_render_pass("UI display encode"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = final_output,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                    .texture = ui_overlay_target,
                    .stages = RHI::PipelineStage::FragmentShader,
                    .access = RHI::AccessFlags::ShaderRead,
                })
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = presentation_extent.x,
                    .height = presentation_extent.y,
                })
                .set_execute([this, ui_overlay_target, output_format, presentation_extent,
                              ui_display_settings, &submission](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .x = 0.0f,
                        .y = 0.0f,
                        .width = static_cast<f32>(presentation_extent.x),
                        .height = static_cast<f32>(presentation_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = presentation_extent.x,
                        .height = presentation_extent.y,
                    });
                    return record_tonemap(
                        pass,
                        context.texture(ui_overlay_target).default_view,
                        output_format,
                        ui_display_settings,
                        submission.transient_bind_groups,
                        true);
                });
        }

        if (submission.render_graph.debug_overlay) {
            const f64 seconds = duration<f64>(steady_clock::now() - declare_graph_start).count();
            current_frame_cpu_stage_timings_ms.emplace_back("declare render graph", seconds * 1000.0);
        }
        if (gpu_timing_enabled) {


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


        acquired_image_guard.resolved = true;


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


            ScopedRendererStageTimer timer{"issue present", &current_frame_cpu_stage_timings_ms};


            const RHI::PresentationResolution presentation = device->presentation_resolution(record.rhi_swapchain);
            RHI::FenceHandle completion_fence{};
            if (presentation.supports_completion_fence) {
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
            record.pending_present = presentation_coordinator_for(presentation.present_queue_is_compute).enqueue(
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


            if (Core::RendererResult drained = drain_pending_present(record, &current_frame_cpu_stage_timings_ms);
                !drained.has_value()) {
                return drained;
            }
        }


        if (gpu_timing_enabled) {
            slot.cpu_timing.stage_timings = std::move(current_frame_cpu_stage_timings_ms);
            slot.cpu_timing.stage_timings.insert(slot.cpu_timing.stage_timings.end(),
                                                 submission.pre_dispatch_stage_timings_ms.begin(),
                                                 submission.pre_dispatch_stage_timings_ms.end());
        }
        return {};
    }

    /// Finds or creates the frame deferred targets required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param extent `extent` value used by the operation.
    /// @param formats Format used for the resource, render target, or conversion.
    /// @param samples `samples` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Destroys the frame deferred targets identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Finds or creates the frame bloom targets required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param extent `extent` value used by the operation.
    /// @param requested_levels `requested_levels` value used by the operation.
    /// @param downsample_ratio `downsample_ratio` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Destroys the frame bloom targets identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Finds or creates the frame composite target required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param extent `extent` value used by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Destroys the frame composite target identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Finds or creates the frame GPU timing target required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param required_pass_count Number of elements or operations to process.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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


        destroy_frame_gpu_timing_target(slot);


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

    /// Destroys the frame GPU timing target identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_frame_gpu_timing_target(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_gpu_timing_target");
        if (RHI::RhiDevice *device = rhi_device(); device != nullptr && slot.gpu_timing.query_set) {
            device->destroy_query_set(slot.gpu_timing.query_set);
        }
        slot.gpu_timing = {};
    }

    /// Finds or creates the frame pregraph GPU timing target required by the operation.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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

    /// Destroys the frame pregraph GPU timing target identified by the supplied parameters.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_frame_pregraph_gpu_timing_target(FrameInFlight &slot) noexcept {
        ZoneScopedN("Renderer::destroy_frame_pregraph_gpu_timing_target");
        if (RHI::RhiDevice *device = rhi_device(); device != nullptr && slot.pregraph_gpu_timing_query_set) {
            device->destroy_query_set(slot.pregraph_gpu_timing_query_set);
        }
        slot.pregraph_gpu_timing_query_set = {};
        slot.pregraph_gpu_timing_pending.clear();
    }

    /// Reclaims frame slot using the supplied arguments and current state.
    ///
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param destroy_retired_presentation `destroy_retired_presentation` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
        slot.scene_tlas = {};
        slot.spectral_scene_instances = {};
        slot.spectral_materials = {};
        slot.spectral_material_textures.clear();
        slot.spectral_frame_constants = {};
        slot.spectral_photon_constants = {};
        slot.spectral_scene_bounds = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
    }

    /// Drains frames in flight using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::drain_frames_in_flight(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::drain_frames_in_flight");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }

        device->wait_idle();
        for (FrameInFlight &slot : record.frames_in_flight) {


            reclaim_frame_slot(slot, true);
            slot.submitted = false;


            if (slot.fence) {
                if (auto reset = device->reset_fences(span<const RHI::FenceHandle>{&slot.fence, 1}); !reset) {
                    Foundation::log_warn("Failed to reset drained frame fence: {}", reset.error().message);
                }
            }
        }
    }

    /// Reclaims completed presentation fences using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Reclaims completed retired presentations using the supplied arguments and current state.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Destroys the retired presentations identified by the supplied parameters.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Performs the maybe flush retired swapchains operation for `Renderer` using the supplied arguments.
    ///
    /// @param record `record` value used by the operation.
    /// @param opportunistic `opportunistic` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::maybe_flush_retired_swapchains(WindowSurfaceRecord &record, bool opportunistic) noexcept {
        ZoneScopedN("Renderer::maybe_flush_retired_swapchains");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr && device->is_enabled(RHI::Feature::SwapchainMaintenance)) {


            reclaim_completed_retired_presentations(record);
            bool has_fence_less_retirement = false;
            for (const FrameInFlight &slot : record.frames_in_flight) {
                if (!slot.retired_swapchains.empty()) {
                    has_fence_less_retirement = true;
                    break;
                }
            }
            if (!has_fence_less_retirement) {
                return;
            }
        }

        usize retired_count = 0;
        for (const FrameInFlight &slot : record.frames_in_flight) {
            retired_count += slot.retired_swapchains.size();
        }


        const usize threshold = opportunistic ? 1 : retired_swapchain_flush_threshold;
        if (retired_count < threshold) {
            return;
        }
        ScopedRendererStageTimer timer{"flush retired swapchains"};
        drain_frames_in_flight(record);
    }

    /// Destroys the RHI presentation resources identified by the supplied parameters.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_rhi_presentation_resources(WindowSurfaceRecord &record) noexcept {
        ZoneScopedN("Renderer::destroy_rhi_presentation_resources");
        if (RHI::RhiDevice *device = rhi_device()) {


            (void)drain_pending_present(record, nullptr);


            drain_frames_in_flight(record);


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

    /// Waits for idle to complete.
    ///
    /// @return Returns the current wait idle value.
    /// @note This function does not throw exceptions.
    void Renderer::wait_idle() noexcept {
        ZoneScopedN("Renderer::wait_idle");
        if (graphics_backend_) {
            graphics_backend_->wait_idle();
        }
    }

    /// Returns the current or globally available feature negotiation report value.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const RHI::FeatureNegotiationReport *Renderer::feature_negotiation_report() const noexcept {
        ZoneScopedN("Renderer::feature_negotiation_report");
        const RHI::RhiDevice *device = rhi_device();
        return device != nullptr ? &device->feature_negotiation_report() : nullptr;
    }

    /// Returns the current GPU info.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<Core::GpuInfo> Renderer::gpu_info() const {
        ZoneScopedN("Renderer::gpu_info");
        if (!graphics_backend_) {
            return std::nullopt;
        }
        return graphics_backend_->gpu_info();
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    RHI::RhiDevice *Renderer::rhi_device() noexcept {
        ZoneScopedN("Renderer::rhi_device");
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const RHI::RhiDevice *Renderer::rhi_device() const noexcept {
        ZoneScopedN("Renderer::rhi_device");
        return graphics_backend_ ? graphics_backend_->rhi_device() : nullptr;
    }

} // namespace SFT::Renderer
