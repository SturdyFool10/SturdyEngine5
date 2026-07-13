module;

#pragma region Imports
#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <glm/mat4x4.hpp>
#pragma endregion

module Sturdy.Renderer;

import :Renderer;
import :Scene;
import :RenderGraph;
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

    Renderer::Renderer() {
        auto backend = Core::create_vulkan_backend();
        graphics_backend_.reset(backend.release());
    }

    Renderer::~Renderer() {
        wait_idle();
        destroy_all_resources();
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Renderer::initialize(
        const Core::RendererCreateInfo &create_info) {
        Foundation::log_info("[szdiag] vep_data_addr={}", static_cast<const void *>(&tonemap_.vertex_entry_point));
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
        for (auto it = window_surfaces_.begin(); it != window_surfaces_.end(); ++it) {
            if (it->surface == surface) {
                destroy_rhi_presentation_resources(*it);
                if (graphics_backend_) {
                    graphics_backend_->destroy_window_surface(surface);
                }
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
        const bool previous_debug_fallback = debug_fallback_enabled_;
        const glm::mat4 previous_view_projection = frame_view_projection_;
        debug_fallback_enabled_ = false;
        frame_view_projection_ = desc.view.camera.projection * desc.view.camera.view;
        frame_draws_.clear();

        for (const SceneRenderable &renderable : desc.view.renderables) {
            if ((renderable.visibility_mask & desc.view.visibility_mask) == 0) {
                continue;
            }
            if (mesh(renderable.mesh) == nullptr) {
                debug_fallback_enabled_ = previous_debug_fallback;
                frame_view_projection_ = previous_view_projection;
                frame_draws_.clear();
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Scene renderable references an unknown mesh.");
            }
            if (material_instance(renderable.material) == nullptr) {
                debug_fallback_enabled_ = previous_debug_fallback;
                frame_view_projection_ = previous_view_projection;
                frame_draws_.clear();
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Scene renderable references an unknown material instance.");
            }
            frame_draws_.push_back(RenderItem{
                .mesh = renderable.mesh,
                .material = renderable.material,
                .world_transform = renderable.world_transform,
                .stable_id = renderable.stable_id,
                .sort_key = renderable.sort_key,
            });
        }

        Core::RendererResult result = render_frame(desc.surface, desc.frame);
        frame_draws_.clear();
        frame_view_projection_ = previous_view_projection;
        debug_fallback_enabled_ = previous_debug_fallback;
        return result;
    }

    Core::RendererResult Renderer::render_frame(Core::RenderSurfaceHandle surface,
                                                const Core::FrameInput &frame) {
        WindowSurfaceRecord *record = window_surface(surface);
        if (record == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is not registered.");
        }

        // Dev-time shader hot-reload: pick up any edited `.slang` file and rebuild the affected material
        // templates before recording. Cheap when nothing changed (a directory stat); the reload path
        // itself does the one sanctioned wait_idle (see plans/shader-variants-and-hot-reload.md).
        poll_shader_hot_reload();

        if (frame_draws_.empty() && debug_fallback_enabled_) {
            if (Core::RendererResult debug_resources = ensure_debug_scene_resources(); !debug_resources.has_value()) {
                return debug_resources;
            }
            frame_draws_.push_back(RenderItem{.mesh = debug_scene_.mesh, .material = debug_scene_.material_instance});
        }

        auto submit_frame = [&]() -> Core::RendererResult {
            return render_frame_rhi(*record, frame);
        };

        Core::RendererResult result = submit_frame();
        if (result.has_value() || result.error().code != Core::GraphicsBackendErrorCode::DeviceLost) {
            frame_draws_.clear();
            return result;
        }

        Core::RendererResult recovery = recover_from_device_loss();
        if (!recovery.has_value()) {
            frame_draws_.clear();
            return recovery;
        }

        record = window_surface(surface);
        if (record == nullptr) {
            frame_draws_.clear();
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer surface is unavailable after device-loss recovery.");
        }
        Core::RendererResult retry = render_frame_rhi(*record, frame);
        frame_draws_.clear();
        return retry;
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
        if (mesh(debug_scene_.mesh) != nullptr && material_template(debug_scene_.material_template) != nullptr &&
            material_instance(debug_scene_.material_instance) != nullptr) {
            return {};
        }

        destroy_debug_scene_resources();

        // Source-backed so a live edit to Shaders/geometry_color.slang hot-reloads the triangle
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
            Core::Slang::ShaderSource::from_file("Shaders/geometry_color.slang", "geometry_color"),
            shader_options, "renderer debug vertex-color material template");
        if (!material_template_handle) {
            return unexpected(material_template_handle.error());
        }
        debug_scene_.material_template = *material_template_handle;

        auto material_instance_handle = create_material_instance(debug_scene_.material_template, "renderer debug vertex-color material");
        if (!material_instance_handle) {
            destroy_debug_scene_resources();
            return unexpected(material_instance_handle.error());
        }
        debug_scene_.material_instance = *material_instance_handle;

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
            destroy_debug_scene_resources();
            return unexpected(mesh_handle.error());
        }
        debug_scene_.mesh = *mesh_handle;
        return {};
    }

    void Renderer::destroy_debug_scene_resources() noexcept {
        if (debug_scene_.mesh) {
            destroy_mesh(debug_scene_.mesh);
        }
        if (debug_scene_.material_instance) {
            destroy_material_instance(debug_scene_.material_instance);
        }
        if (debug_scene_.material_template) {
            destroy_material_template(debug_scene_.material_template);
        }
        debug_scene_ = {};
    }

    Core::RendererResult Renderer::record_render_item(RHI::RenderPassEncoder &pass,
                                                      const RenderItem &item,
                                                      RHI::Format color_format,
                                                      RHI::Format depth_format,
                                                      u64 frame_index) {
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

        auto pipeline = material_pipeline_for(*material_template_resource, color_format, depth_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        pass.set_pipeline(*pipeline);

        const SceneDrawConstants draw_constants{
            .view_projection = frame_view_projection_,
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

        // In-flight frames still reference the current swapchain images (imported into their graphs) and
        // the depth target we're about to destroy. Drain them before tearing those down — a resize/rebuild
        // is rare and is the sanctioned heavy-wait exception, not the per-frame path.
        drain_frames_in_flight();

        const RHI::SwapchainHandle old_swapchain = record.rhi_swapchain;
        const RHI::TextureHandle old_depth_texture = record.depth_texture;
        const RHI::TextureViewHandle old_depth_view = record.depth_view;

        RHI::SwapchainDesc swapchain_desc{
            .surface = record.rhi_surface,
            .width = extent.width,
            .height = extent.height,
            .format = RHI::Format::BGRA8UnormSrgb,
            .present_mode = RHI::PresentMode::Fifo,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::TransferDst,
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
        record.depth_texture = {};
        record.depth_view = {};
        record.swapchain_extent = extent;
        record.rhi_swapchain_dirty = false;
        if (old_depth_view) {
            device->destroy_texture_view(old_depth_view);
        }
        if (old_depth_texture) {
            device->destroy_texture(old_depth_texture);
        }
        if (old_swapchain) {
            device->destroy_swapchain(old_swapchain);
        }
        return ensure_rhi_depth_resources(record);
    }

    Core::RendererResult Renderer::render_frame_rhi(WindowSurfaceRecord &record,
                                                    const Core::FrameInput &frame) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer RHI device is unavailable.");
        }

        // N-buffered in-flight ring, keyed by frame_index so it tracks the material system's per-frame
        // UBO slot (frame_index % N). (Re)size on the first frame or after a capability change (device-loss
        // recovery clears the ring).
        const u32 frame_count = capabilities_.max_frames_in_flight == 0 ? 1u : capabilities_.max_frames_in_flight;
        if (frames_in_flight_.size() != frame_count) {
            frames_in_flight_.assign(frame_count, FrameInFlight{});
        }
        FrameInFlight &slot = frames_in_flight_[frame.frame_index % frame_count];

        // Backpressure — the one sanctioned per-frame CPU wait (plans/async-submission-model.md). Waits on
        // the *specific* frame that last used this ring slot (frame_count frames ago), never a full-device
        // stall, capping the CPU to frame_count frames ahead of the GPU. Once its fence signals, that
        // frame's command buffer / transient targets / bind groups are safe to reclaim and its material
        // UBO slot is free to overwrite.
        if (slot.submitted) {
            if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&slot.fence, 1}, true); !waited) {
                return unexpected(graphics_error_from_rhi(waited.error(), "wait in-flight frame fence"));
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
        if (Core::RendererResult depth_resources = ensure_rhi_depth_resources(record); !depth_resources.has_value()) {
            return depth_resources;
        }

        // Pre-warm the tonemap shader/pipeline before recording so the render-pass callback only mints a
        // bind group + draws — never compiles a shader or builds a pipeline mid command-buffer recording.
        if (Core::RendererResult tonemap_ready = ensure_tonemap_resources(); !tonemap_ready.has_value()) {
            return tonemap_ready;
        }
        if (auto tonemap_pipeline = tonemap_pipeline_for(RHI::Format::BGRA8UnormSrgb); !tonemap_pipeline) {
            return unexpected(tonemap_pipeline.error());
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

        RenderGraph graph;
        const RenderGraphTextureHandle swapchain_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = texture.texture,
            .default_view = texture.view,
            .format = RHI::Format::BGRA8UnormSrgb,
            .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::Present,
            .final_stage = RHI::PipelineStage::None,
            .final_access = RHI::AccessFlags::None,
            .label = "swapchain color",
        });
        // HDR intermediate: the scene renders here, then the tonemap pass resolves it to the swapchain.
        const RenderGraphTextureHandle scene_color = graph.create_texture(RenderGraphTextureDesc{
            .format = scene_color_format,
            .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .label = "scene color (HDR)",
        });
        const RenderGraphTextureHandle depth_texture = graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = record.depth_texture,
            .default_view = record.depth_view,
            .format = record.depth_format,
            .extent = RHI::Extent3D{.width = extent.width, .height = extent.height, .depth_or_layers = 1},
            .initial_layout = RHI::TextureLayout::Undefined,
            .initial_stage = RHI::PipelineStage::None,
            .initial_access = RHI::AccessFlags::None,
            .final_layout = RHI::TextureLayout::DepthStencilAttachment,
            .final_stage = RHI::PipelineStage::LateFragmentTests,
            .final_access = RHI::AccessFlags::DepthStencilAttachmentWrite,
            .label = "main depth",
        });

        graph.add_render_pass("renderer main pass")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = scene_color,
                .load_op = RHI::LoadOp::Clear,
                .store_op = RHI::StoreOp::Store,
                .clear_color = RHI::ClearColor{0.01f, 0.015f, 0.025f, 1.0f},
            })
            .set_depth_stencil_attachment(RenderGraphDepthStencilAttachmentDesc{
                .texture = depth_texture,
                .depth_load_op = RHI::LoadOp::Clear,
                .depth_store_op = RHI::StoreOp::Store,
                .clear_value = RHI::ClearDepthStencil{.depth = 1.0f, .stencil = 0},
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.width, .height = extent.height})
            .set_execute([this, &record, extent, frame](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(extent.width),
                    .height = static_cast<f32>(extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = extent.width, .height = extent.height});
                for (const RenderItem &item : frame_draws_) {
                    if (Core::RendererResult recorded = record_render_item(pass, item, scene_color_format, record.depth_format, frame.frame_index);
                        !recorded.has_value()) {
                        return recorded;
                    }
                }
                return {};
            });

        // Tonemap post-process: sample the HDR scene color and resolve it to the swapchain. Replaces the
        // old straight blit — the first real post-process slot of the deferred pipeline.
        constexpr RHI::Format swapchain_format = RHI::Format::BGRA8UnormSrgb;
        graph.add_render_pass("tonemap")
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = swapchain_texture,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = scene_color,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = extent.width, .height = extent.height})
            .set_execute([this, extent, scene_color](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(extent.width),
                    .height = static_cast<f32>(extent.height),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = extent.width, .height = extent.height});
                const RenderGraphTextureAccess source = context.texture(scene_color);
                return record_tonemap(pass, source.default_view, swapchain_format);
            });

        if (Core::RendererResult graph_result = graph.execute(*device, **encoder); !graph_result.has_value()) {
            return graph_result;
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
        if (auto submitted = device->submit(submit_desc); !submitted) {
            graph.destroy_transient_resources(*device);
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit RHI frame"));
        }

        // The frame is now in flight. Hand its GPU resources to the ring slot for fence-gated cleanup —
        // deliberately NO wait here (the whole point of the async model). They are reclaimed the next time
        // this slot comes round, after its fence has signaled.
        slot.command_buffer = *command_buffer;
        graph.take_transient_resources(slot.transient_textures, slot.transient_texture_views);
        slot.transient_bind_groups = std::move(frame_transient_bind_groups_);
        frame_transient_bind_groups_.clear();
        slot.submitted = true;

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

    void Renderer::reclaim_frame_slot(FrameInFlight &slot) noexcept {
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
            if (slot.command_buffer) {
                device->destroy_command_buffer(slot.command_buffer);
            }
        }
        slot.transient_bind_groups.clear();
        slot.transient_texture_views.clear();
        slot.transient_textures.clear();
        slot.command_buffer = {};
    }

    void Renderer::drain_frames_in_flight() noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        // Sanctioned heavy wait (teardown / swapchain rebuild), never the per-frame path.
        device->wait_idle();
        for (FrameInFlight &slot : frames_in_flight_) {
            if (slot.submitted) {
                reclaim_frame_slot(slot);
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
