#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <utility>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
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

namespace SFT::Renderer {

    namespace {

        constexpr f64 renderer_stage_hitch_threshold_seconds = 0.050;
        constexpr f64 swapchain_resize_settle_seconds = 0.075;

        class ScopedRendererStageTimer {
          public:
            explicit ScopedRendererStageTimer(const char *stage) noexcept
                : stage_(stage), start_(steady_clock::now()) {}

            ~ScopedRendererStageTimer() noexcept {
                const f64 seconds = duration<f64>(steady_clock::now() - start_).count();
                if (seconds >= renderer_stage_hitch_threshold_seconds) {
                    Foundation::log_warn("Renderer stage '{}' took {}", stage_, Foundation::human_readable_time(seconds));
                }
            }

          private:
            const char *stage_;
            steady_clock::time_point start_;
        };

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

    void Renderer::on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept {
        if (graphics_backend_) {
            graphics_backend_->on_surface_resize_needed(surface);
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
        submission.view_projection = desc.view.camera.projection * desc.view.camera.view;
        submission.debug_label = desc.view.debug_label != nullptr ? desc.view.debug_label : string{};

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

        if (submission.draws.empty()) {
            if (Core::RendererResult debug_resources = ensure_debug_scene_resources(); !debug_resources.has_value()) {
                return debug_resources;
            }
            auto guard = debug_scene_.lock();
            submission.draws.push_back(RenderItem{.mesh = guard->mesh, .material = guard->material_instance});
        }

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

    Core::RendererResult Renderer::ensure_debug_scene_resources() {
        auto guard = debug_scene_.lock();
        if (mesh(guard->mesh) != nullptr && material_template(guard->material_template) != nullptr &&
            material_instance(guard->material_instance) != nullptr) {
            return {};
        }

        destroy_debug_scene_resources_locked(*guard);

        // Source-backed so a live edit to the deferred G-buffer shader hot-reloads the triangle
        // (poll_shader_hot_reload() at the top of render_frame drives it).
        const Core::Slang::ShaderCompileOptions shader_options{
            .targets = {Core::Slang::ShaderTarget{}},
            .entry_points = {
                Core::Slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = Core::Slang::ShaderStage::Vertex},
                Core::Slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = Core::Slang::ShaderStage::Fragment},
            },
            .search_paths = {},
            .macros = {},
            .optimization = Core::Slang::ShaderOptimizationLevel::Default,
            .allow_glsl_syntax = false,
            .skip_spirv_validation = false,
            .enable_effect_annotations = false,
        };
        auto material_template_handle = create_material_template_from_source(
            Core::Slang::ShaderSource::from_file("Shaders/gbuffer_geometry.slang", "gbuffer_geometry"),
            shader_options, "renderer debug gbuffer material template");
        if (!material_template_handle) {
            return unexpected(material_template_handle.error());
        }
        guard->material_template = *material_template_handle;

        auto material_instance_handle = create_material_instance(guard->material_template, "renderer debug vertex-color material");
        if (!material_instance_handle) {
            destroy_debug_scene_resources_locked(*guard);
            return unexpected(material_instance_handle.error());
        }
        guard->material_instance = *material_instance_handle;

        constexpr array<GeometryVertex, 3> vertices{
            GeometryVertex{.position = {0.0f, -0.55f, 0.0f}, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
            GeometryVertex{.position = {0.55f, 0.45f, 0.0f}, .color = {0.0f, 1.0f, 0.0f, 1.0f}},
            GeometryVertex{.position = {-0.55f, 0.45f, 0.0f}, .color = {0.0f, 0.0f, 1.0f, 1.0f}},
        };
        constexpr array<u32, 3> indices{0, 1, 2};
        auto mesh_handle = create_mesh(span<const GeometryVertex>{vertices.data(), vertices.size()},
                                       span<const u32>{indices.data(), indices.size()},
                                       "renderer debug triangle mesh");
        if (!mesh_handle) {
            destroy_debug_scene_resources_locked(*guard);
            return unexpected(mesh_handle.error());
        }
        guard->mesh = *mesh_handle;
        return {};
    }

    void Renderer::destroy_debug_scene_resources() noexcept {
        auto guard = debug_scene_.lock();
        destroy_debug_scene_resources_locked(*guard);
    }

    void Renderer::destroy_debug_scene_resources_locked(DebugSceneResources &scene) noexcept {
        if (scene.mesh) {
            destroy_mesh(scene.mesh);
        }
        if (scene.material_instance) {
            destroy_material_instance(scene.material_instance);
        }
        if (scene.material_template) {
            destroy_material_template(scene.material_template);
        }
        scene = {};
    }

    Core::RendererResult Renderer::record_render_item(RHI::RenderPassEncoder &pass,
                                                      const RenderItem &item,
                                                      span<const RHI::Format> color_formats,
                                                      RHI::Format depth_format,
                                                      u64 frame_index,
                                                      const glm::mat4 &view_projection) {
        MeshResource *mesh_resource = mesh(item.mesh);
        if (mesh_resource == nullptr || !mesh_resource->gpu_resident || !mesh_resource->vertex_buffer) {
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

        auto pipeline = material_pipeline_for(*material_template_resource, color_formats, depth_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        pass.set_pipeline(*pipeline);

        const SceneDrawConstants draw_constants{
            .view_projection = view_projection,
            .model = item.world_transform,
        };
        pass.set_push_constants(RHI::ShaderStage::Vertex, 0,
                                std::as_bytes(span<const SceneDrawConstants>{&draw_constants, 1}));

        if (!material_resource->frames.empty()) {
            const u32 frame_slot = static_cast<u32>(frame_index % material_resource->frames.size());
            auto bind_groups = prepare_material_frame(*material_resource, frame_slot);
            if (!bind_groups) {
                return unexpected(bind_groups.error());
            }
            for (usize i = 0; i < bind_groups->size() && i < material_template_resource->bind_group_layout_sets.size(); ++i) {
                pass.set_bind_group(material_template_resource->bind_group_layout_sets[i], (*bind_groups)[i]);
            }
        }

        pass.set_vertex_buffer(0, mesh_resource->vertex_buffer);
        if (mesh_resource->index_buffer && !mesh_resource->indices.empty()) {
            pass.set_index_buffer(mesh_resource->index_buffer, RHI::IndexFormat::Uint32);
            pass.draw_indexed(RHI::DrawIndexedArgs{.index_count = static_cast<u32>(mesh_resource->indices.size())});
        } else {
            pass.draw(RHI::DrawArgs{.vertex_count = static_cast<u32>(mesh_resource->vertices.size())});
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
                destroy_frame_deferred_targets(old_slot);
            }
            record.frames_in_flight.assign(frame_count, FrameInFlight{});
        }
        FrameInFlight &slot = record.frames_in_flight[frame.frame_index % frame_count];

        // Backpressure — the one sanctioned per-frame CPU wait (plans/async-submission-model.md). Waits on
        // the *specific* frame that last used this ring slot (frame_count frames ago), never a full-device
        // stall, capping the CPU to frame_count frames ahead of the GPU. Once its fence signals, that
        // frame's command buffer / transient targets / bind groups are safe to reclaim and its material
        // UBO slot is free to overwrite.
        if (slot.submitted) {
            {
                ScopedRendererStageTimer timer{"wait in-flight frame fence"};
                if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true); !waited) {
                    return unexpected(graphics_error_from_rhi(waited.error(), "wait in-flight frame fence"));
                }
            }
            if (auto reset = device->reset_fences(span<const RHI::FenceHandle>{&slot.fence, 1}); !reset) {
                return unexpected(graphics_error_from_rhi(reset.error(), "reset in-flight frame fence"));
            }
            reclaim_frame_slot(slot);
            slot.submitted = false;
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
        Core::Extent2D render_extent = extent;
        const bool size_changed = extent.width != record.swapchain_extent.width ||
            extent.height != record.swapchain_extent.height;
        const bool should_recreate = record.rhi_swapchain_dirty || size_changed;
        bool defer_size_recreate_this_frame = false;
        if (should_recreate) {
            // During live resize, Linux/Wayland can deliver many intermediate extents. Rebuilding for
            // each one can make vkCreateSwapchainKHR block inside the WSI/compositor path for hundreds
            // of milliseconds. If we already have a swapchain and only the size is changing, wait until
            // the extent has been stable for a tiny window, then rebuild once for the coalesced size.
            if (record.rhi_swapchain && size_changed) {
                const steady_clock::time_point now = steady_clock::now();
                const bool new_pending_extent = record.pending_swapchain_extent.width != extent.width ||
                    record.pending_swapchain_extent.height != extent.height;
                if (new_pending_extent) {
                    record.pending_swapchain_extent = extent;
                    record.pending_swapchain_extent_since = now;
                    defer_size_recreate_this_frame = true;
                } else {
                    const f64 stable_seconds = duration<f64>(now - record.pending_swapchain_extent_since).count();
                    defer_size_recreate_this_frame = stable_seconds < swapchain_resize_settle_seconds;
                }

                if (defer_size_recreate_this_frame) {
                    // Keep rendering/presenting the old swapchain while the compositor is still sending
                    // intermediate resize extents. The window manager scales/composites the old image, which
                    // is visually smoother than presenting nothing and avoids repeatedly blocking in WSI.
                    render_extent = record.swapchain_extent;
                }
            }

            if (!defer_size_recreate_this_frame) {
                ScopedRendererStageTimer timer{"recreate swapchain"};
                if (Core::RendererResult recreated = recreate_rhi_swapchain(record, frame.frame_index, extent); !recreated.has_value()) {
                    return recreated;
                }
                record.pending_swapchain_extent = {};
                record.pending_swapchain_extent_since = {};
                render_extent = record.swapchain_extent;
            }
        }
        if (!record.rhi_swapchain) {
            return {};
        }
        if (Core::RendererResult depth_resources = ensure_rhi_depth_resources(record); !depth_resources.has_value()) {
            return depth_resources;
        }
        if (Core::RendererResult scene_gpu_data = prepare_scene_gpu_data(frame.frame_index, submission); !scene_gpu_data.has_value()) {
            return scene_gpu_data;
        }
        if (Core::RendererResult deferred_targets = ensure_frame_deferred_targets(slot, render_extent, submission.deferred_formats); !deferred_targets.has_value()) {
            return deferred_targets;
        }

        // Pre-warm fullscreen post-process shaders/pipelines before recording so render-pass callbacks only
        // mint bind groups + draw — never compile shaders or build pipelines mid command-buffer recording.
        if (Core::RendererResult deferred_lighting_ready = ensure_deferred_lighting_resources(); !deferred_lighting_ready.has_value()) {
            return deferred_lighting_ready;
        }
        if (auto lighting_pipeline = deferred_lighting_pipeline_for(submission.deferred_formats.lighting); !lighting_pipeline) {
            return unexpected(lighting_pipeline.error());
        }
        if (Core::RendererResult tonemap_ready = ensure_tonemap_resources(); !tonemap_ready.has_value()) {
            return tonemap_ready;
        }
        if (auto tonemap_pipeline = tonemap_pipeline_for(RHI::Format::BGRA8UnormSrgb); !tonemap_pipeline) {
            return unexpected(tonemap_pipeline.error());
        }

        auto acquired = [&]() {
            ScopedRendererStageTimer timer{"acquire swapchain texture"};
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

        RenderGraph graph;
        const RenderGraphTextureHandle swapchain_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = texture.texture,
            .default_view = texture.view,
            .format = RHI::Format::BGRA8UnormSrgb,
            .extent = RHI::Extent3D{.width = render_extent.width, .height = render_extent.height, .depth_or_layers = 1},
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
        // HDR deferred lighting target: tonemap samples this instead of the swapchain seeing geometry directly.
        const RenderGraphTextureHandle scene_lighting = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = slot.deferred_targets.scene_lighting,
            .default_view = slot.deferred_targets.scene_lighting_view,
            .format = submission.deferred_formats.lighting,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "deferred scene lighting (HDR)",
        });
        const RenderGraphTextureHandle depth_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = record.depth_texture,
            .default_view = record.depth_view,
            .format = record.depth_format,
            .extent = frame_extent,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::DepthStencilAttachment,
            .final_stage = RHI::PipelineStage::LateFragmentTests,
            .final_access = RHI::AccessFlags::DepthStencilAttachmentWrite,
            .label = "deferred depth",
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
                .clear_color = RHI::ClearColor{0.5f, 0.5f, 1.0f, 0.0f},
            })
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = gbuffer_material,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            })
            .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                .texture = depth_texture,
                .depth_load_op = RHI::LoadOp::Clear,
                .depth_store_op = RHI::StoreOp::Store,
                .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
            .set_execute([this, &record, &submission, render_extent, frame](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(render_extent.width),
                    .height = static_cast<f32>(render_extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                const array<RHI::Format, 3> gbuffer_formats{
                    submission.deferred_formats.albedo,
                    submission.deferred_formats.normal,
                    submission.deferred_formats.material,
                };
                for (const RenderItem &item : submission.draws) {
                    if (Core::RendererResult recorded = record_render_item(pass, item,
                                                                          span<const RHI::Format>{gbuffer_formats.data(), gbuffer_formats.size()},
                                                                          record.depth_format, frame.frame_index,
                                                                          submission.view_projection);
                        !recorded.has_value()) {
                        return recorded;
                    }
                }
                return {};
            });

        graph.add_render_pass("deferred lighting")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_lighting,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.01f, 0.015f, 0.025f, 1.0f},
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = gbuffer_albedo,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = gbuffer_normal,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = gbuffer_material,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
            .set_execute([this, &submission, render_extent, gbuffer_albedo, gbuffer_normal, gbuffer_material](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(render_extent.width),
                    .height = static_cast<f32>(render_extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                const RenderGraphTextureAccess albedo = context.texture(gbuffer_albedo);
                const RenderGraphTextureAccess normal = context.texture(gbuffer_normal);
                const RenderGraphTextureAccess material = context.texture(gbuffer_material);
                return record_deferred_lighting(pass, albedo.default_view, normal.default_view, material.default_view,
                                                submission.deferred_formats.lighting, submission.transient_bind_groups);
            });

        // Tonemap post-process: sample HDR deferred lighting and resolve it to the swapchain.
        constexpr RHI::Format swapchain_format = RHI::Format::BGRA8UnormSrgb;
        graph.add_render_pass("tonemap")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = swapchain_texture,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = scene_lighting,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
            .set_execute([this, &submission, render_extent, scene_lighting](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(render_extent.width),
                    .height = static_cast<f32>(render_extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                const RenderGraphTextureAccess source = context.texture(scene_lighting);
                return record_tonemap(pass, source.default_view, swapchain_format, submission.transient_bind_groups);
            });

        // Debug HUD text overlay: scene label, renderable count, camera position, FPS, frame index —
        // drawn last, straight onto the swapchain, on top of the tonemapped scene.
        const f32 overlay_fps = frame.delta_seconds > 0.0 ? static_cast<f32>(1.0 / frame.delta_seconds) : 0.0f;
        const array<string, 5> overlay_lines{
            submission.debug_label.empty() ? string{"Scene"} : submission.debug_label,
            std::format("Renderables: {}", submission.draws.size()),
            std::format("Camera: ({:.2f}, {:.2f}, {:.2f})", submission.camera.world_position.x,
                       submission.camera.world_position.y, submission.camera.world_position.z),
            std::format("FPS: {:.1f}", overlay_fps),
            std::format("Frame: {}", frame.frame_index),
        };
        graph.add_render_pass("debug text overlay")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = swapchain_texture,
                .load_op = RHI::LoadOp::Load,
                .store_op = RHI::StoreOp::Store,
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height})
            .set_execute([this, &submission, render_extent, overlay_lines](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(render_extent.width),
                    .height = static_cast<f32>(render_extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = render_extent.width, .height = render_extent.height});
                const glm::vec2 viewport_size{static_cast<f32>(render_extent.width), static_cast<f32>(render_extent.height)};
                return record_text_overlay(pass, swapchain_format, span<const string>{overlay_lines.data(), overlay_lines.size()},
                                           glm::vec2{10.0f, 10.0f}, viewport_size, submission.transient_bind_groups);
            });

        {
            ScopedRendererStageTimer timer{"execute render graph"};
            if (Core::RendererResult graph_result = graph.execute(*device, **encoder); !graph_result.has_value()) {
                return graph_result;
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
            ScopedRendererStageTimer timer{"submit RHI frame"};
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
        graph.take_transient_resources(slot.transient_textures, slot.transient_texture_views);
        slot.transient_bind_groups = std::move(submission.transient_bind_groups);
        slot.submitted = true;

        auto presented = [&]() {
            ScopedRendererStageTimer timer{"present RHI frame"};
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
            slot.deferred_targets.extent.width == extent.width &&
            slot.deferred_targets.extent.height == extent.height &&
            slot.deferred_targets.formats.albedo == formats.albedo &&
            slot.deferred_targets.formats.normal == formats.normal &&
            slot.deferred_targets.formats.material == formats.material &&
            slot.deferred_targets.formats.lighting == formats.lighting;
        if (matches) {
            return {};
        }

        destroy_frame_deferred_targets(slot);

        auto create_target = [&](RHI::Format format, const char *label) -> Core::RendererExpected<std::pair<RHI::TextureHandle, RHI::TextureViewHandle>> {
            auto texture = device->create_texture(RHI::TextureDesc{
                .dimension = RHI::TextureDimension::Dim2D,
                .format = format,
                .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
                .mip_levels = 1,
                .samples = RHI::SampleCount::X1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
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

        auto albedo = create_target(formats.albedo, "persistent deferred gbuffer albedo");
        if (!albedo) return unexpected(albedo.error());
        auto normal = create_target(formats.normal, "persistent deferred gbuffer normal");
        if (!normal) {
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(normal.error());
        }
        auto material = create_target(formats.material, "persistent deferred gbuffer material");
        if (!material) {
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(material.error());
        }
        auto lighting = create_target(formats.lighting, "persistent deferred scene lighting");
        if (!lighting) {
            device->destroy_texture_view(material->second);
            device->destroy_texture(material->first);
            device->destroy_texture_view(normal->second);
            device->destroy_texture(normal->first);
            device->destroy_texture_view(albedo->second);
            device->destroy_texture(albedo->first);
            return unexpected(lighting.error());
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
            .scene_lighting = lighting->first,
            .scene_lighting_view = lighting->second,
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
            destroy_target(slot.deferred_targets.scene_lighting, slot.deferred_targets.scene_lighting_view);
        }
        slot.deferred_targets = {};
    }

    void Renderer::reclaim_frame_slot(FrameInFlight &slot, bool destroy_retired_presentation) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (RHI::BindGroupHandle group : slot.transient_bind_groups) {
                if (group) {
                    device->destroy_bind_group(group);
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
        // Sanctioned heavy wait (teardown / swapchain rebuild), never the per-frame path.
        device->wait_idle();
        for (FrameInFlight &slot : record.frames_in_flight) {
            if (slot.submitted) {
                reclaim_frame_slot(slot, true);
                slot.submitted = false;
            }
            // Leave the fence allocated but unsignaled so the slot is immediately reusable — wait_idle
            // above left every submitted fence signaled, and vkQueueSubmit needs an unsignaled one.
            if (slot.fence) {
                if (auto reset = device->reset_fences(span<const RHI::FenceHandle>{&slot.fence, 1}); !reset) {
                    Foundation::log_warn("Failed to reset drained frame fence: {}", reset.error().message);
                }
            }
        }
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
