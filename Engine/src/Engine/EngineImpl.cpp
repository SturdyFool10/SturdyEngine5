#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <expected>
#include <format>

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#pragma endregion

#include <Core/Core.hpp>
#include <Engine/EngineModule.hpp>
#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>

using std::format;
using std::span;
using std::string_view;

using std::unexpected;

namespace RendererApi = SFT::Renderer;

namespace SFT::Engine {

    namespace {
        [[nodiscard]] RendererApi::ToneMappingOperator lower_tone_mapping(ToneMappingOperator operation) noexcept {
            switch (operation) {
                case ToneMappingOperator::None: return RendererApi::ToneMappingOperator::None;
                case ToneMappingOperator::Reinhard: return RendererApi::ToneMappingOperator::Reinhard;
                case ToneMappingOperator::Exponential: return RendererApi::ToneMappingOperator::Exponential;
                case ToneMappingOperator::Agx: return RendererApi::ToneMappingOperator::Agx;
                case ToneMappingOperator::HermiteSpline: return RendererApi::ToneMappingOperator::HermiteSpline;
                case ToneMappingOperator::PsychoV: return RendererApi::ToneMappingOperator::PsychoV;
            }
            return RendererApi::ToneMappingOperator::Agx;
        }

        [[nodiscard]] RendererApi::AgxLook lower_agx_look(AgxLook look) noexcept {
            switch (look) {
                case AgxLook::None: return RendererApi::AgxLook::None;
                case AgxLook::Punchy: return RendererApi::AgxLook::Punchy;
                case AgxLook::Golden: return RendererApi::AgxLook::Golden;
            }
            return RendererApi::AgxLook::None;
        }
    } // namespace

    // The API-selection switch point now lives inside SFT::Renderer::Renderer. Engine owns the
    // high-level renderer, not the raw graphics backend.
    Engine::Engine() {
        ecs_world_.bind_resource(platform_event_inbox_);
        ecs_world_.bind_resource(window_events_);
        ecs_world_.bind_resource(keyboard_events_);
        ecs_world_.bind_resource(text_input_events_);
        ecs_world_.bind_resource(mouse_move_events_);
        ecs_world_.bind_resource(mouse_button_events_);
        ecs_world_.bind_resource(mouse_wheel_events_);
        ecs_world_.bind_resource(window_state_events_);
        ecs_world_.bind_resource(window_state_);

        update_schedule_.add_system(
            [](Ecs::WriteResource<PlatformEventInbox> inbox,
               Ecs::EventWriter<WindowEvent> window_events,
               Ecs::EventWriter<KeyboardEvent> keyboard_events,
               Ecs::EventWriter<TextInputEvent> text_events,
               Ecs::EventWriter<MouseMoveEvent> mouse_move_events,
               Ecs::EventWriter<MouseButtonEvent> mouse_button_events,
               Ecs::EventWriter<MouseWheelEvent> mouse_wheel_events,
               Ecs::EventWriter<WindowStateEvent> window_state_events) noexcept {
                const vector<WindowEvent> pending = inbox->drain();
                for (const WindowEvent &queued : pending) {
                    window_events.send(queued);
                    const Platform::Windowing::WindowEvent &event = queued.event;
                    switch (event.kind) {
                        case Platform::Windowing::WindowEventKind::KeyPressed:
                        case Platform::Windowing::WindowEventKind::KeyReleased:
                            keyboard_events.send(KeyboardEvent{
                                .window = queued.window,
                                .key = event.keyboard.key,
                                .scancode = event.keyboard.scancode,
                                .modifiers = event.keyboard.modifiers,
                                .action = event.kind == Platform::Windowing::WindowEventKind::KeyPressed
                                              ? ButtonAction::Pressed
                                              : ButtonAction::Released,
                                .repeat = event.keyboard.repeat,
                            });
                            break;
                        case Platform::Windowing::WindowEventKind::TextInput:
                            text_events.send(TextInputEvent{.window = queued.window, .text = event.text});
                            break;
                        case Platform::Windowing::WindowEventKind::MouseMoved:
                            mouse_move_events.send(MouseMoveEvent{.window = queued.window, .mouse = event.mouse_move});
                            break;
                        case Platform::Windowing::WindowEventKind::MouseButtonPressed:
                        case Platform::Windowing::WindowEventKind::MouseButtonReleased:
                            mouse_button_events.send(MouseButtonEvent{
                                .window = queued.window,
                                .mouse = event.mouse_button,
                                .action = event.kind == Platform::Windowing::WindowEventKind::MouseButtonPressed
                                              ? ButtonAction::Pressed
                                              : ButtonAction::Released,
                            });
                            break;
                        case Platform::Windowing::WindowEventKind::MouseWheel:
                            mouse_wheel_events.send(MouseWheelEvent{.window = queued.window, .wheel = event.mouse_wheel});
                            break;
                        default:
                            window_state_events.send(WindowStateEvent{
                                .window = queued.window,
                                .kind = event.kind,
                                .position = event.position,
                                .resize = event.resize,
                            });
                            break;
                    }
                }
            });
    }

    Engine::~Engine() = default;

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::initialize(Platform::Windowing::Window &window,
                                                                         const EngineConfig &config) {
        if (initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                         "Engine renderer is already initialized."});
        }

        // Reflect every shader on disk before the graphics backend exists, so the rest of startup
        // can see entry points, bindings, and parameter layouts without having generated any
        // target bytecode yet.
        shaders_ = Core::Slang::discover_shaders(config.shaders_directory, shader_compiler_);

        auto wsi_extensions = window.required_vulkan_instance_extensions();
        if (!wsi_extensions) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::InitializationFailed,
                format("Failed to query Vulkan WSI extensions from window: {}", wsi_extensions.error().message),
            });
        }

        Core::RendererCreateInfo renderer_info{};
        renderer_info.features = config.features;
        renderer_info.app_name = config.app_name;
        renderer_info.window = &window;
        renderer_info.wsi_extensions = std::move(*wsi_extensions);
        // Hand the backend the shaders we reflected above; it owns compiling them to its native
        // format. shaders_ outlives this call, so the non-owning span stays valid.
        renderer_info.uncompiled_shaders = shaders_;

        auto surface = renderer_.initialize(renderer_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        initialized_ = true;
        config_ = config;
        primary_window_ = &window;
        capabilities_ = renderer_.capabilities();
        return *surface;
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::add_window(Platform::Windowing::Window &window,
                                                                         u32 desired_frames_in_flight) {
        if (!initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                         "Engine renderer must be initialized before adding another window."});
        }
        return renderer_.create_window_surface(window, desired_frames_in_flight);
    }

    void Engine::remove_window(Core::RenderSurfaceHandle surface) noexcept {
        renderer_.destroy_window_surface(surface);
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                              Platform::Windowing::Window &new_window,
                                                                              u32 desired_frames_in_flight) {
        remove_window(old_surface);
        return add_window(new_window, desired_frames_in_flight);
    }

    void Engine::on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        renderer_.on_surface_resize_needed(surface, extent);
    }

    Core::RendererResult Engine::set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                           const Core::PresentationSettings &settings) {
        return renderer_.set_presentation_settings(surface, settings);
    }

    Core::RendererExpected<Core::RuntimeSettingsChangeResult>
    Engine::apply_runtime_settings(Core::RenderSurfaceHandle primary_surface,
                                   const EngineConfig &settings) {
        if (!initialized_ || primary_window_ == nullptr) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                         "Engine must be initialized before applying runtime settings."});
        }

        Core::RuntimeSettingsChangeResult result{};
        const Core::PresentationSettings &old_presentation = config_.features.presentation;
        const Core::PresentationSettings &new_presentation = settings.features.presentation;

        const bool hdr_changed =
            static_cast<bool>(old_presentation.hdr_enabled) != static_cast<bool>(new_presentation.hdr_enabled);
        const RHI::RhiDevice *active_rhi = renderer_.rhi_device();
        const bool hdr_colorspace_enabled = active_rhi != nullptr &&
                                            active_rhi->is_extension_enabled(RHI::ExtensionId{"vulkan", "VK_EXT_swapchain_colorspace", 1});
        const bool hdr_metadata_enabled = active_rhi != nullptr &&
                                          active_rhi->is_extension_enabled(RHI::ExtensionId{"vulkan", "VK_EXT_hdr_metadata", 1});
        const bool hdr_requires_backend_rebuild = hdr_changed &&
                                                  (!static_cast<bool>(new_presentation.hdr_enabled) || !hdr_colorspace_enabled || !hdr_metadata_enabled);

        const bool presentation_changed =
            static_cast<bool>(old_presentation.vsync) != static_cast<bool>(new_presentation.vsync) ||
            old_presentation.present_mode != new_presentation.present_mode ||
            old_presentation.swapchain_image_count != new_presentation.swapchain_image_count ||
            (hdr_changed && !hdr_requires_backend_rebuild);

        const bool backend_features_changed =
            static_cast<bool>(config_.features.raytracing) != static_cast<bool>(settings.features.raytracing) ||
            static_cast<bool>(config_.features.prefer_async_compute) != static_cast<bool>(settings.features.prefer_async_compute) ||
            config_.features.required_rhi_features != settings.features.required_rhi_features ||
            config_.features.optional_rhi_features != settings.features.optional_rhi_features ||
            config_.features.desired_frames_in_flight != settings.features.desired_frames_in_flight ||
            static_cast<bool>(config_.features.enable_native_access_extension) != static_cast<bool>(settings.features.enable_native_access_extension);

        const string_view old_app_name = config_.app_name != nullptr ? string_view{config_.app_name} : string_view{};
        const string_view new_app_name = settings.app_name != nullptr ? string_view{settings.app_name} : string_view{};
        const bool app_name_changed = old_app_name != new_app_name;

        if (!presentation_changed && !backend_features_changed && !app_name_changed &&
            config_.shaders_directory == settings.shaders_directory) {
            result.mode = Core::RuntimeSettingApplyMode::NoChange;
            result.message = "Runtime settings already match the active engine configuration.";
            return result;
        }

        if (backend_features_changed || hdr_requires_backend_rebuild || app_name_changed || config_.shaders_directory != settings.shaders_directory) {
            auto wsi_extensions = primary_window_->required_vulkan_instance_extensions();
            if (!wsi_extensions) {
                return unexpected(Core::GraphicsBackendError{
                    Core::GraphicsBackendErrorCode::InitializationFailed,
                    format("Failed to query Vulkan WSI extensions while applying runtime settings: {}", wsi_extensions.error().message),
                });
            }

            if (config_.shaders_directory != settings.shaders_directory) {
                shaders_ = Core::Slang::discover_shaders(settings.shaders_directory, shader_compiler_);
            }

            Core::RendererCreateInfo renderer_info{};
            renderer_info.features = settings.features;
            renderer_info.app_name = settings.app_name;
            renderer_info.window = primary_window_;
            renderer_info.wsi_extensions = std::move(*wsi_extensions);
            renderer_info.uncompiled_shaders = shaders_;

            if (Core::RendererResult rebuilt = renderer_.reconfigure_backend(renderer_info); !rebuilt.has_value()) {
                return unexpected(rebuilt.error());
            }
            config_ = settings;
            capabilities_ = renderer_.capabilities();
            result.mode = Core::RuntimeSettingApplyMode::BackendRecreated;
            result.message = hdr_requires_backend_rebuild
                                 ? "Runtime settings changed Vulkan extension requirements; the graphics backend was rebuilt with the requested extension set without restarting the game."
                                 : "Runtime settings required graphics backend/device recreation and were applied without restarting the game.";
            return result;
        }

        if (presentation_changed) {
            if (Core::RendererResult applied = renderer_.set_presentation_settings(primary_surface, new_presentation); !applied.has_value()) {
                return unexpected(applied.error());
            }
            config_.features.presentation = new_presentation;
            result.mode = Core::RuntimeSettingApplyMode::SurfaceRecreated;
            result.message = "Presentation settings were applied; the swapchain will be recreated on the next rendered frame.";
            return result;
        }

        result.mode = Core::RuntimeSettingApplyMode::HotApplied;
        result.message = "Runtime settings were applied.";
        return result;
    }

    Core::RendererResult Engine::render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) {
        const PreparedRenderFrame prepared = prepare_render_frame(surface, frame);
        return render(prepared);
    }

    PreparedRenderFrame Engine::prepare_render_frame(Core::RenderSurfaceHandle surface,
                                                     const Core::FrameInput &frame,
                                                     const RenderFrameParameters &parameters) {
        render_frame_requests_.begin_frame();
        render_extraction_schedule_.run(ecs_world_);

        if (RenderGraphResult graph_validation = parameters.render_graph.validate(); !graph_validation) {
            Foundation::log_error("Invalid high-level render graph: {} Using its normalized safe form.",
                                  graph_validation.error().message);
        }
        RenderGraphDescription graph = parameters.render_graph.normalized().description();
        graph.resolution_scale = std::clamp(
            graph.resolution_scale * parameters.camera.render_scale(), 0.1f, 2.0f);
        if (!graph.scene.background_color) {
            graph.scene.background_color = parameters.camera.clear_color();
        }

        return PreparedRenderFrame{
            .surface = surface,
            .frame = frame,
            .camera = parameters.camera.renderer_view(),
            .lighting = SFT::Renderer::SceneLighting{
                .ambient_radiance = parameters.lighting.ambient_radiance,
                .exposure = parameters.lighting.exposure * parameters.camera.exposure_multiplier(),
            },
            .deferred_formats = SFT::Renderer::DeferredTargetFormats{},
            .renderables = render_frame_requests_.finish_frame(),
            .render_graph = graph,
            .custom_post_processes = parameters.custom_post_processes,
            .visibility_mask = parameters.camera.culling_mask(),
            .debug_label = parameters.debug_label,
        };
    }

    Core::RendererResult Engine::render(const PreparedRenderFrame &frame) {
        RendererApi::RenderFrameDesc desc{};
        desc.surface = frame.surface;
        desc.frame = frame.frame;
        desc.view.camera = frame.camera;
        desc.view.lighting = frame.lighting;
        desc.view.deferred_formats = frame.deferred_formats;
        desc.view.render_graph = RendererApi::RenderGraphSettings{
            .render_scene = frame.render_graph.scene.enabled,
            .deferred_lighting = frame.render_graph.lighting.enabled,
            .bloom = frame.render_graph.bloom.enabled,
            .tone_mapping = frame.render_graph.tone_mapping.enabled,
            .debug_overlay = frame.render_graph.debug_overlay.enabled,
            .wait_for_completion = frame.render_graph.execution_mode == RenderGraphExecutionMode::WaitForCompletion,
            .resolution_scale = frame.render_graph.resolution_scale,
            .background_color = frame.render_graph.scene.background_color.value_or(glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}),
            .background_intensity = frame.render_graph.lighting.background_intensity,
            .bloom_threshold = frame.render_graph.bloom.threshold,
            .bloom_soft_knee = frame.render_graph.bloom.soft_knee,
            .bloom_intensity = frame.render_graph.bloom.intensity,
            .bloom_scatter = frame.render_graph.bloom.scatter,
            .bloom_max_levels = frame.render_graph.bloom.max_levels,
            .tone_mapping_operator = lower_tone_mapping(frame.render_graph.tone_mapping.operation),
            .tone_mapping_exposure = frame.render_graph.tone_mapping.exposure,
            .tone_mapping_white_point = frame.render_graph.tone_mapping.white_point,
            .tone_mapping_saturation = frame.render_graph.tone_mapping.saturation,
            .tone_mapping_hdr_paper_white_nits = frame.render_graph.tone_mapping.hdr_paper_white_nits,
            .tone_mapping_hdr_peak_nits = frame.render_graph.tone_mapping.hdr_peak_nits,
            .agx_look = lower_agx_look(frame.render_graph.tone_mapping.agx.look),
            .hermite_toe_strength = frame.render_graph.tone_mapping.hermite_spline.toe_strength,
            .hermite_toe_length = frame.render_graph.tone_mapping.hermite_spline.toe_length,
            .hermite_shoulder_strength = frame.render_graph.tone_mapping.hermite_spline.shoulder_strength,
            .hermite_shoulder_length = frame.render_graph.tone_mapping.hermite_spline.shoulder_length,
            .hermite_shoulder_angle = frame.render_graph.tone_mapping.hermite_spline.shoulder_angle,
            .psychov_highlights = frame.render_graph.tone_mapping.psycho_v.highlights,
            .psychov_shadows = frame.render_graph.tone_mapping.psycho_v.shadows,
            .psychov_contrast = frame.render_graph.tone_mapping.psycho_v.contrast,
            .psychov_purity_scale = frame.render_graph.tone_mapping.psycho_v.purity_scale,
            .psychov_gamut_compression = frame.render_graph.tone_mapping.psycho_v.gamut_compression,
            .psychov_gamut_compression_use_bt2020 = frame.render_graph.tone_mapping.psycho_v.gamut_compression_use_bt2020,
            .psychov_compression = frame.render_graph.tone_mapping.psycho_v.compression,
            .psychov_adapted_gray_bt709 = frame.render_graph.tone_mapping.psycho_v.adapted_gray_bt709,
            .psychov_background_gray_bt709 = frame.render_graph.tone_mapping.psycho_v.background_gray_bt709,
            .custom_post_processes = frame.custom_post_processes,
        };
        desc.view.visibility_mask = frame.visibility_mask;
        if (frame.renderables) {
            desc.view.renderables = span<const RendererApi::SceneRenderable>{
                frame.renderables->data(),
                frame.renderables->size()};
        }
        desc.view.debug_label = frame.debug_label;
        return renderer_.render_frame(desc);
    }

    void Engine::wait_idle() noexcept {
        renderer_.wait_idle();
    }

    std::optional<Core::GpuInfo> Engine::gpu_info() const {
        return renderer_.gpu_info();
    }

    Core::EngineBackend *Engine::graphics_backend() noexcept {
        return renderer_.graphics_backend();
    }

    RHI::RhiDevice *Engine::rhi_device() noexcept {
        return renderer_.rhi_device();
    }

} // namespace SFT::Engine
