#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <expected>
#include <format>
#include <memory>

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <Engine/EngineModule.hpp>
#include <WindowManager/WindowManager.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>

using std::format;
using std::optional;
using std::shared_ptr;
using std::span;
using std::string_view;
using std::vector;

using std::unexpected;

namespace RendererApi = SFT::Renderer;

namespace SFT::Engine {

    namespace {
        /// Performs the lower tone mapping operation for `Engine` using the supplied arguments.
        ///
        /// @param operation `operation` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererApi::ToneMappingOperator lower_tone_mapping(ToneMappingOperator operation) noexcept {
            switch (operation) {
                case ToneMappingOperator::None:
                    return RendererApi::ToneMappingOperator::None;
                case ToneMappingOperator::Reinhard:
                    return RendererApi::ToneMappingOperator::Reinhard;
                case ToneMappingOperator::Exponential:
                    return RendererApi::ToneMappingOperator::Exponential;
                case ToneMappingOperator::Agx:
                    return RendererApi::ToneMappingOperator::Agx;
                case ToneMappingOperator::HermiteSpline:
                    return RendererApi::ToneMappingOperator::HermiteSpline;
                case ToneMappingOperator::PsychoV:
                    return RendererApi::ToneMappingOperator::PsychoV;
            }
            return RendererApi::ToneMappingOperator::Agx;
        }

        /// Performs the lower scene integrator operation for `Engine` using the supplied arguments.
        ///
        /// @param integrator `integrator` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererApi::SpectralRenderMode lower_scene_integrator(SceneIntegrator integrator) noexcept {
            switch (integrator) {
                case SceneIntegrator::RasterDeferred:
                    return RendererApi::SpectralRenderMode::RasterDeferred;
                case SceneIntegrator::ShadowOnly:
                    return RendererApi::SpectralRenderMode::ShadowOnly;
                case SceneIntegrator::ReflectionOnly:
                    return RendererApi::SpectralRenderMode::ReflectionOnly;
                case SceneIntegrator::AmbientOcclusionOnly:
                    return RendererApi::SpectralRenderMode::AmbientOcclusionOnly;
                case SceneIntegrator::ShadowAndTransmission:
                    return RendererApi::SpectralRenderMode::ShadowAndTransmission;
                case SceneIntegrator::FullPathTracing:
                    return RendererApi::SpectralRenderMode::FullPathTracing;
            }
            return RendererApi::SpectralRenderMode::RasterDeferred;
        }

        /// Performs the lower agx look operation for `Engine` using the supplied arguments.
        ///
        /// @param look `look` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererApi::AgxLook lower_agx_look(AgxLook look) noexcept {
            switch (look) {
                case AgxLook::None:
                    return RendererApi::AgxLook::None;
                case AgxLook::Punchy:
                    return RendererApi::AgxLook::Punchy;
                case AgxLook::Golden:
                    return RendererApi::AgxLook::Golden;
            }
            return RendererApi::AgxLook::None;
        }

    } // namespace


    /// Constructs a `Engine` in its default state.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Engine::Engine() {
        ecs_world_.bind_resource(platform_event_inbox_);
        ecs_world_.bind_resource(window_events_);
        ecs_world_.bind_resource(keyboard_events_);
        ecs_world_.bind_resource(text_input_events_);
        ecs_world_.bind_resource(text_editing_events_);
        ecs_world_.bind_resource(mouse_move_events_);
        ecs_world_.bind_resource(mouse_button_events_);
        ecs_world_.bind_resource(mouse_wheel_events_);
        ecs_world_.bind_resource(window_state_events_);
        ecs_world_.bind_resource(window_state_);
        ecs_world_.bind_resource(input_state_);
        ecs_world_.bind_resource(window_requests_);
        ecs_world_.bind_resource(frame_time_);
        ecs_world_.bind_resource(time_scale_);
        ecs_world_.bind_resource(ui_pointer_state_);
        ecs_world_.bind_resource(ui_text_input_state_);
        ecs_world_.bind_resource(ui_context_);
        ecs_world_.bind_resource(ui_image_cache_);
        ecs_world_.bind_resource(ui_svg_cache_);


        window_events_.reserve(256);
        mouse_move_events_.reserve(256);

        update_schedule_.add_system(
            [](Ecs::WriteResource<PlatformEventInbox> inbox,
               Ecs::EventWriter<WindowEvent> window_events,
               Ecs::EventWriter<KeyboardEvent> keyboard_events,
               Ecs::EventWriter<TextInputEvent> text_events,
               Ecs::EventWriter<TextEditingEvent> text_editing_events,
               Ecs::EventWriter<MouseMoveEvent> mouse_move_events,
               Ecs::EventWriter<MouseButtonEvent> mouse_button_events,
               Ecs::EventWriter<MouseWheelEvent> mouse_wheel_events,
               Ecs::EventWriter<WindowStateEvent> window_state_events) noexcept {
                const vector<WindowEvent> pending = inbox->drain();
                for (const WindowEvent &queued : pending) {
                    window_events.send(queued);
                    const WindowManager::WindowEvent &event = queued.event;
                    switch (event.kind) {
                        case WindowManager::WindowEventKind::KeyPressed:
                        case WindowManager::WindowEventKind::KeyReleased:
                            keyboard_events.send(KeyboardEvent{
                                .window = queued.window,
                                .key = event.keyboard.key,
                                .scancode = event.keyboard.scancode,
                                .modifiers = event.keyboard.modifiers,
                                .key_code = event.keyboard.key_code,
                                .action = event.kind == WindowManager::WindowEventKind::KeyPressed
                                              ? ButtonAction::Pressed
                                              : ButtonAction::Released,
                                .repeat = event.keyboard.repeat,
                                .timestamp_ns = event.timestamp_ns,
                            });
                            break;
                        case WindowManager::WindowEventKind::TextInput:
                            text_events.send(TextInputEvent{.window = queued.window, .text = event.text, .timestamp_ns = event.timestamp_ns});
                            break;
                        case WindowManager::WindowEventKind::TextEditing:
                            text_editing_events.send(TextEditingEvent{.window = queued.window, .text = event.editing, .timestamp_ns = event.timestamp_ns});
                            break;
                        case WindowManager::WindowEventKind::MouseMoved:
                            mouse_move_events.send(MouseMoveEvent{.window = queued.window, .mouse = event.mouse_move, .timestamp_ns = event.timestamp_ns});
                            break;
                        case WindowManager::WindowEventKind::MouseButtonPressed:
                        case WindowManager::WindowEventKind::MouseButtonReleased:
                            mouse_button_events.send(MouseButtonEvent{
                                .window = queued.window,
                                .mouse = event.mouse_button,
                                .action = event.kind == WindowManager::WindowEventKind::MouseButtonPressed
                                              ? ButtonAction::Pressed
                                              : ButtonAction::Released,
                                .timestamp_ns = event.timestamp_ns,
                            });
                            break;
                        case WindowManager::WindowEventKind::MouseWheel:
                            mouse_wheel_events.send(MouseWheelEvent{.window = queued.window, .wheel = event.mouse_wheel, .timestamp_ns = event.timestamp_ns});
                            break;
                        default:
                            window_state_events.send(WindowStateEvent{
                                .window = queued.window,
                                .kind = event.kind,
                                .position = event.position,
                                .resize = event.resize,
                                .timestamp_ns = event.timestamp_ns,
                            });
                            break;
                    }
                }
            });


        update_schedule_.add_system(
            [](Ecs::WriteResource<UiPointerState> pointer,
               Ecs::EventReader<MouseMoveEvent> mouse_move,
               Ecs::EventReader<MouseButtonEvent> mouse_button,
               Ecs::EventReader<MouseWheelEvent> mouse_wheel) noexcept {
                for (const MouseMoveEvent &event : mouse_move.read()) {
                    pointer->set_position({event.mouse.x, event.mouse.y});
                }
                for (const MouseButtonEvent &event : mouse_button.read()) {
                    if (event.mouse.button_code == WindowManager::MouseButton::Left) {
                        pointer->set_position({event.mouse.x, event.mouse.y});
                        pointer->set_down(event.action == ButtonAction::Pressed);
                    }
                }
                for (const MouseWheelEvent &event : mouse_wheel.read()) {


                    pointer->add_scroll_delta({-event.wheel.x, event.wheel.y});
                }
            });


        update_schedule_.add_system(
            [](Ecs::WriteResource<UiTextInputState> text_input,
               Ecs::EventReader<KeyboardEvent> keyboard,
               Ecs::EventReader<TextInputEvent> text,
               Ecs::EventReader<TextEditingEvent> text_editing) noexcept {
                for (const KeyboardEvent &event : keyboard.read()) {
                    text_input->apply_key(event);
                }
                for (const TextInputEvent &event : text.read()) {
                    text_input->apply(event);
                }
                for (const TextEditingEvent &event : text_editing.read()) {
                    text_input->apply(event);
                }
            });


        update_schedule_.add_system(
            [](Ecs::WriteResource<InputState> input,
               Ecs::EventReader<KeyboardEvent> keyboard,
               Ecs::EventReader<TextInputEvent> text,
               Ecs::EventReader<TextEditingEvent> text_editing,
               Ecs::EventReader<MouseMoveEvent> mouse_move,
               Ecs::EventReader<MouseButtonEvent> mouse_button,
               Ecs::EventReader<MouseWheelEvent> mouse_wheel) noexcept {
                input->begin_tick();
                for (const KeyboardEvent &event : keyboard.read()) {
                    input->apply(event);
                }
                for (const TextInputEvent &event : text.read()) {
                    input->apply(event);
                }
                for (const TextEditingEvent &event : text_editing.read()) {
                    input->apply(event);
                }
                for (const MouseMoveEvent &event : mouse_move.read()) {
                    input->apply(event);
                }
                for (const MouseButtonEvent &event : mouse_button.read()) {
                    input->apply(event);
                }
                for (const MouseWheelEvent &event : mouse_wheel.read()) {
                    input->apply(event);
                }
            });
    }

    /// Destroys the `Engine` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    Engine::~Engine() {


        if (initialized_) {
            wait_idle();
            if (RHI::RhiDevice *device = rhi_device()) {
                ui_context_.destroy(*device);
            }
        }
    }

    /// Initializes the `Engine` for use.
    ///
    /// @param window Window used or affected by the operation.
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `GraphicsBackendErrorCode::InitializationFailed`.
    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::initialize(WindowManager::Window &window,
                                                                         const EngineConfig &config) {
        if (initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                         "Engine renderer is already initialized."});
        }

        const Foundation::Stopwatch stopwatch;

#if !defined(STURDY_PLATFORM_WEB)
        gpu_inventory_ = Core::enumerate_gpu_inventory(
            RHI::InstanceDesc{.application_name = config.app_name, .engine_name = "SturdyEngine5", .headless = true});
        for (const RHI::BackendDiscoveryFailure &failure : gpu_inventory_.failures) {
            Foundation::log_warn("GPU discovery through {} failed: {}", RHI::backend_type_name(failure.backend), failure.message);
        }
        for (const RHI::PhysicalGpu &gpu : gpu_inventory_.gpus) {
            std::string api_names;
            for (const RHI::GpuApiSupport &api : gpu.api_support) {
                if (!api_names.empty()) {
                    api_names += ", ";
                }
                api_names += RHI::backend_type_name(api.adapter.backend);
            }
            Foundation::log_info("Found GPU: {} (available APIs: {})", gpu.name, api_names);
        }
#endif


        shaders_ = Core::Slang::discover_shaders(
            config.shaders_directory,
            shader_compiler_,
            Core::Slang::ShaderCompileOptions{},
            config.enable_shader_disk_cache);

        Core::RendererCreateInfo renderer_info{};
        renderer_info.backend = config.graphics_backend;
        renderer_info.physical_device_id = config.graphics_physical_device_id;
        renderer_info.features = config.features;
        renderer_info.app_name = config.app_name;
        renderer_info.window = &window;


        renderer_info.uncompiled_shaders = shaders_;
        renderer_info.enable_shader_disk_cache = config.enable_shader_disk_cache;

        auto surface = renderer_.initialize(renderer_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        initialized_ = true;
        config_ = config;
        primary_window_ = &window;
        capabilities_ = renderer_.capabilities();
        Foundation::log_info("Engine initialized in {}", stopwatch.elapsed_human());
        return *surface;
    }

    /// Adds window using the supplied arguments and current state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::add_window(WindowManager::Window &window,
                                                                         u32 desired_frames_in_flight) {
        if (!initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                         "Engine renderer must be initialized before adding another window."});
        }
        return renderer_.create_window_surface(window, desired_frames_in_flight);
    }

    /// Removes the window from its owning collection or system.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Engine::remove_window(Core::RenderSurfaceHandle surface) noexcept {
        renderer_.destroy_window_surface(surface);
    }

    /// Creates a offscreen render target from the supplied parameters.
    ///
    /// @param description Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RenderTargetHandle> Engine::create_offscreen_render_target(
        const OffscreenRenderTargetDescription &description) {
        auto target = renderer_.create_offscreen_render_target(RendererApi::OffscreenRenderTargetDescription{
            .extent = Core::Extent2D{description.width, description.height},
            .label = description.label,
        });
        if (!target) {
            return unexpected(target.error());
        }
        return RenderTargetHandle{.value = target->value};
    }

    /// Destroys the offscreen render target identified by the supplied parameters.
    ///
    /// @param target `target` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Engine::destroy_offscreen_render_target(RenderTargetHandle target) noexcept {
        renderer_.destroy_offscreen_render_target(RendererApi::OffscreenRenderTargetHandle{.value = target.value});
    }

    /// Performs the offscreen render target description operation for `Engine` using the supplied arguments.
    ///
    /// @param target `target` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<OffscreenRenderTargetDescription> Engine::offscreen_render_target_description(
        RenderTargetHandle target) const {
        const optional<RendererApi::OffscreenRenderTargetDescription> description =
            renderer_.offscreen_render_target_description(
                RendererApi::OffscreenRenderTargetHandle{.value = target.value});
        if (!description) {
            return std::nullopt;
        }
        return OffscreenRenderTargetDescription{
            .width = description->extent.x,
            .height = description->extent.y,
            .label = description->label,
        };
    }

    /// Performs the offscreen render target texture operation for `Engine` using the supplied arguments.
    ///
    /// @param target `target` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RendererApi::TextureHandle Engine::offscreen_render_target_texture(RenderTargetHandle target) const noexcept {
        return renderer_.offscreen_render_target_texture(
            RendererApi::OffscreenRenderTargetHandle{.value = target.value});
    }

    /// Recreates window using the supplied arguments and current state.
    ///
    /// @param old_surface Surface used or affected by the operation.
    /// @param new_window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                              WindowManager::Window &new_window,
                                                                              u32 desired_frames_in_flight) {
        remove_window(old_surface);
        return add_window(new_window, desired_frames_in_flight);
    }

    /// Handles the on surface resize needed callback and updates the associated platform state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Engine::on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        renderer_.on_surface_resize_needed(surface, extent);
    }

    /// Sets the presentation settings for this `Engine`.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Engine::set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                           const Core::PresentationSettings &settings) {
        return renderer_.set_presentation_settings(surface, settings);
    }

    /// Returns `surface`'s current presentation policy (vsync/HDR/transparent-composition/...).
    ///
    /// @param surface Surface to query.
    ///
    /// @return The value last accepted by set_presentation_settings() for this surface, or the
    ///         app-wide default it was created with if never overridden; default-constructed
    ///         Core::PresentationSettings{} if `surface` isn't registered.
    /// @note This function does not throw exceptions.
    Core::PresentationSettings Engine::presentation_settings(Core::RenderSurfaceHandle surface) const noexcept {
        return renderer_.presentation_settings(surface);
    }

    /// Queries HDR capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RHI::RhiExpected<RHI::SurfaceHdrCapabilityQuery> Engine::query_hdr_capabilities(
        Core::RenderSurfaceHandle surface) const {
        return renderer_.query_hdr_capabilities(surface);
    }

    /// Updates HDR content light level from the supplied values.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param update `update` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RHI::RhiResult Engine::update_hdr_content_light_level(Core::RenderSurfaceHandle surface,
                                                          const RHI::HdrContentLightLevelUpdate &update) {
        return renderer_.update_hdr_content_light_level(surface, update);
    }

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RHI::PresentationResolution Engine::presentation_resolution(Core::RenderSurfaceHandle surface) const noexcept {
        return renderer_.presentation_resolution(surface);
    }

    /// Applies runtime settings using the supplied arguments and current state.
    ///
    /// @param primary_surface Surface used or affected by the operation.
    /// @param settings Configuration values controlling the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `GraphicsBackendErrorCode::Unsupported`, `GraphicsBackendErrorCode::InitializationFailed`.
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

        const bool hdr_enabled_changed =
            static_cast<bool>(old_presentation.hdr_enabled) != static_cast<bool>(new_presentation.hdr_enabled);


        const bool hdr_color_space_setting_changed =
            old_presentation.hdr_color_space != new_presentation.hdr_color_space;
        const bool hdr_color_space_changed =
            static_cast<bool>(new_presentation.hdr_enabled) && hdr_color_space_setting_changed;
        const bool hdr_changed = hdr_enabled_changed || hdr_color_space_changed;
        const RHI::RhiDevice *active_rhi = renderer_.rhi_device();


        const bool hdr_colorspace_enabled = active_rhi != nullptr &&
                                            (active_rhi->backend_type() != RHI::BackendType::Vulkan ||
                                             active_rhi->is_extension_enabled(RHI::ExtensionId{"vulkan", "VK_EXT_swapchain_colorspace", 1}));
        if (hdr_enabled_changed && static_cast<bool>(new_presentation.hdr_enabled) &&
            !hdr_colorspace_enabled) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::Unsupported,
                "HDR presentation is unavailable because VK_EXT_swapchain_colorspace is not supported.",
            });
        }
        const bool presentation_changed =
            old_presentation.vsync != new_presentation.vsync ||
            old_presentation.variable_refresh != new_presentation.variable_refresh ||
            old_presentation.latency != new_presentation.latency ||
            old_presentation.preference != new_presentation.preference ||
            static_cast<bool>(old_presentation.transparent_composition) !=
                static_cast<bool>(new_presentation.transparent_composition) ||
            old_presentation.swapchain_image_count != new_presentation.swapchain_image_count ||
            static_cast<bool>(old_presentation.allow_present_from_compute) != static_cast<bool>(new_presentation.allow_present_from_compute) ||
            hdr_changed;

        const bool backend_features_changed =
            static_cast<bool>(config_.features.raytracing) != static_cast<bool>(settings.features.raytracing) ||
            static_cast<bool>(config_.features.prefer_async_compute) != static_cast<bool>(settings.features.prefer_async_compute) ||
            config_.features.required_rhi_features != settings.features.required_rhi_features ||
            config_.features.optional_rhi_features != settings.features.optional_rhi_features ||
            config_.features.desired_frames_in_flight != settings.features.desired_frames_in_flight ||
            static_cast<bool>(config_.features.enable_native_access_extension) != static_cast<bool>(settings.features.enable_native_access_extension) ||
            config_.graphics_backend != settings.graphics_backend ||
            config_.graphics_physical_device_id != settings.graphics_physical_device_id;

        const string_view old_app_name = config_.app_name != nullptr ? string_view{config_.app_name} : string_view{};
        const string_view new_app_name = settings.app_name != nullptr ? string_view{settings.app_name} : string_view{};
        const bool app_name_changed = old_app_name != new_app_name;

        if (!presentation_changed && !hdr_color_space_setting_changed &&
            !backend_features_changed && !app_name_changed &&
            config_.shaders_directory == settings.shaders_directory) {
            result.mode = Core::RuntimeSettingApplyMode::NoChange;
            result.message = "Runtime settings already match the active engine configuration.";
            return result;
        }

        if (backend_features_changed || app_name_changed || config_.shaders_directory != settings.shaders_directory) {
            if (config_.shaders_directory != settings.shaders_directory) {
                shaders_ = Core::Slang::discover_shaders(
                    settings.shaders_directory,
                    shader_compiler_,
                    Core::Slang::ShaderCompileOptions{},
                    settings.enable_shader_disk_cache);
            }

            Core::RendererCreateInfo renderer_info{};
            renderer_info.backend = settings.graphics_backend;
            renderer_info.physical_device_id = settings.graphics_physical_device_id;
            renderer_info.features = settings.features;
            renderer_info.app_name = settings.app_name;
            renderer_info.window = primary_window_;
            renderer_info.uncompiled_shaders = shaders_;
            renderer_info.enable_shader_disk_cache = settings.enable_shader_disk_cache;

            if (Core::RendererResult rebuilt = renderer_.reconfigure_backend(renderer_info); !rebuilt.has_value()) {
                return unexpected(rebuilt.error());
            }
            config_ = settings;
            capabilities_ = renderer_.capabilities();
            result.mode = Core::RuntimeSettingApplyMode::BackendRecreated;
            result.message = "Runtime settings required graphics backend/device recreation and were applied without restarting the game.";
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

        if (hdr_color_space_setting_changed) {
            config_.features.presentation = new_presentation;
            result.mode = Core::RuntimeSettingApplyMode::HotApplied;
            result.message = "HDR color-space preference was stored and will take effect when HDR presentation is enabled.";
            return result;
        }

        result.mode = Core::RuntimeSettingApplyMode::HotApplied;
        result.message = "Runtime settings were applied.";
        return result;
    }

    /// Renders the requested content using the current rendering state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Engine::render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) {
        const PreparedRenderFrame prepared = prepare_render_frame(surface, frame);
        return render(prepared);
    }

    /// Prepares render frame for a later operation.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    /// @param parameters `parameters` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    PreparedRenderFrame Engine::prepare_render_frame(Core::RenderSurfaceHandle surface,
                                                     const Core::FrameInput &frame,
                                                     const RenderFrameParameters &parameters) {
        render_frame_requests_.begin_frame();
        light_frame_requests_.begin_frame();
        render_extraction_schedule_.run(ecs_world_);

        const shared_ptr<const LightFrameRequests::ExtractedLights> lights = light_frame_requests_.finish_frame();

        if (RenderGraphResult graph_validation = parameters.render_graph.validate(); !graph_validation) {
            Foundation::log_error("Invalid high-level render graph: {} Using its normalized safe form.",
                                  graph_validation.error().message);
        }
        RenderGraph graph = parameters.render_graph.normalized();
        RenderGraphDescription &graph_settings = graph.description();
        graph_settings.resolution_scale = std::clamp(
            graph_settings.resolution_scale * parameters.camera.render_scale(),
            0.1f,
            2.0f);
        if (!graph_settings.scene.background_color) {
            graph_settings.scene.background_color = parameters.camera.clear_color();
        }

        return PreparedRenderFrame{
            .surface = surface,
            .frame = frame,
            .camera = parameters.camera.renderer_view(),
            .lighting = SFT::Renderer::SceneLighting{
                .ambient_radiance = parameters.lighting.ambient_radiance,
                .exposure = parameters.lighting.exposure * parameters.camera.exposure_multiplier(),
                .sun = lights->sun.value_or(SFT::Renderer::DirectionalLight{}),
                .spot_lights = lights->spot_lights,
                .point_lights = lights->point_lights,
            },
            .deferred_formats = SFT::Renderer::DeferredTargetFormats{},
            .renderables = render_frame_requests_.finish_frame(),
            .gizmo_renderables = render_frame_requests_.finish_gizmo_frame(),
            .render_graph = std::move(graph),
            .ui_overlay = parameters.ui_overlay,
            .visibility_mask = parameters.camera.culling_mask(),
            .debug_label = parameters.debug_label,
        };
    }

    /// Renders the requested content using the current rendering state.
    ///
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Engine::render(const PreparedRenderFrame &frame) {
        optional<RenderGraph> normalized_graph;
        if (RenderGraphResult validation = frame.render_graph.validate(); !validation) {
            Foundation::log_error("Prepared frame contains an invalid high-level render graph: {} Using its normalized safe form.",
                                  validation.error().message);
            normalized_graph.emplace(frame.render_graph.normalized());
        }
        const RenderGraph &executable_graph = normalized_graph ? *normalized_graph : frame.render_graph;
        const RenderGraphDescription &graph = executable_graph.description();
        const vector<RenderGraphPassHandle> presentation_path = executable_graph.presentation_path();
        const auto path_contains = [&executable_graph, &presentation_path](RenderGraphPassKind kind) {
            return std::ranges::any_of(presentation_path, [&executable_graph, kind](RenderGraphPassHandle handle) {
                return handle.index < executable_graph.passes().size() &&
                       executable_graph.passes()[handle.index].kind == kind;
            });
        };
        const bool has_scene = path_contains(RenderGraphPassKind::DeferredScene);
        const bool has_anti_aliasing = path_contains(RenderGraphPassKind::AntiAliasing);
        const bool has_bloom = path_contains(RenderGraphPassKind::Bloom);
        const bool has_tone_mapping = path_contains(RenderGraphPassKind::ToneMapping);
        const bool has_debug_overlay = path_contains(RenderGraphPassKind::DebugOverlay);

        const auto logical = [](RenderGraphTextureHandle handle) {
            return handle ? RendererApi::LogicalRenderGraphTexture{.index = handle.index}
                          : RendererApi::LogicalRenderGraphTexture{};
        };
        enum class TextureDomain : u8 { Unknown,
                                        BeforeBloom,
                                        AfterBloom,
                                        Display };
        vector<TextureDomain> domains(executable_graph.textures().size(), TextureDomain::Unknown);
        vector<bool> live_pass(executable_graph.passes().size(), false);
        vector<bool> presentation_pass(executable_graph.passes().size(), false);
        for (RenderGraphPassHandle handle : presentation_path) {
            if (handle.index < presentation_pass.size()) {
                presentation_pass[handle.index] = true;
            }
        }
        for (RenderGraphPassHandle handle : executable_graph.execution_passes()) {
            if (handle.index < live_pass.size()) {
                live_pass[handle.index] = true;
            }
        }

        RendererApi::CustomGraphProgram custom_graph{
            .texture_count = static_cast<u32>(executable_graph.textures().size()),
        };
        for (const RenderGraphPassDescription &pass : executable_graph.passes()) {
            const TextureDomain input_domain = pass.input && pass.input.index < domains.size()
                                                   ? domains[pass.input.index]
                                                   : TextureDomain::Unknown;
            TextureDomain output_domain = input_domain;
            switch (pass.kind) {
                case RenderGraphPassKind::DeferredScene:
                case RenderGraphPassKind::AntiAliasing:
                    output_domain = TextureDomain::BeforeBloom;
                    break;
                case RenderGraphPassKind::Bloom:
                    output_domain = TextureDomain::AfterBloom;
                    break;
                case RenderGraphPassKind::ToneMapping:
                case RenderGraphPassKind::DebugOverlay:
                    output_domain = TextureDomain::Display;
                    break;
                case RenderGraphPassKind::FullscreenEffect:
                case RenderGraphPassKind::ComputeEffect:
                case RenderGraphPassKind::Copy:
                case RenderGraphPassKind::Present:
                    break;
            }
            if (pass.output && pass.output.index < domains.size()) {
                domains[pass.output.index] = output_domain;
            }

            if (pass.handle.index < presentation_pass.size() && presentation_pass[pass.handle.index]) {
                if (pass.kind == RenderGraphPassKind::DeferredScene) {
                    custom_graph.deferred_scene_output = logical(pass.output);
                } else if (pass.kind == RenderGraphPassKind::AntiAliasing) {
                    custom_graph.anti_aliasing_output = logical(pass.output);
                } else if (pass.kind == RenderGraphPassKind::Bloom) {
                    custom_graph.bloom_output = logical(pass.output);
                }
            }

            if (pass.handle.index >= live_pass.size() || !live_pass[pass.handle.index]) {
                continue;
            }
            const RendererApi::PostProcessStage stage = input_domain == TextureDomain::AfterBloom
                                                            ? RendererApi::PostProcessStage::AfterBloomBeforeToneMap
                                                            : RendererApi::PostProcessStage::BeforeBloom;
            if (pass.kind == RenderGraphPassKind::FullscreenEffect) {
                custom_graph.passes.push_back(RendererApi::CustomGraphPass{
                    .kind = RendererApi::CustomGraphPassKind::RasterEffect,
                    .stage = stage,
                    .input = logical(pass.input),
                    .output = logical(pass.output),
                    .raster = RendererApi::CustomPostProcessEffect{
                        .shader_path = pass.fullscreen_effect.shader_path.string(),
                        .module_name = pass.fullscreen_effect.module_name,
                        .fragment_entry_point = pass.fullscreen_effect.fragment_entry_point,
                        .push_constants = pass.fullscreen_effect.push_constants,
                        .label = pass.fullscreen_effect.label,
                        .stage = stage,
                    },
                    .label = pass.label,
                });
            } else if (pass.kind == RenderGraphPassKind::ComputeEffect) {
                custom_graph.passes.push_back(RendererApi::CustomGraphPass{
                    .kind = RendererApi::CustomGraphPassKind::ComputeEffect,
                    .stage = stage,
                    .input = logical(pass.input),
                    .output = logical(pass.output),
                    .compute = RendererApi::CustomComputeEffect{
                        .shader_path = pass.compute_effect.shader_path.string(),
                        .module_name = pass.compute_effect.module_name,
                        .compute_entry_point = pass.compute_effect.compute_entry_point,
                        .push_constants = pass.compute_effect.push_constants,
                        .label = pass.compute_effect.label,
                    },
                    .label = pass.label,
                });
            } else if (pass.kind == RenderGraphPassKind::Copy) {
                custom_graph.passes.push_back(RendererApi::CustomGraphPass{
                    .kind = RendererApi::CustomGraphPassKind::Copy,
                    .stage = stage,
                    .input = logical(pass.input),
                    .output = logical(pass.output),
                    .label = pass.copy.label,
                });
            }
        }
        for (RenderGraphTextureHandle output : executable_graph.outputs()) {
            custom_graph.outputs.push_back(logical(output));
        }
        for (RenderGraphPassHandle handle : presentation_path) {
            const RenderGraphPassDescription &pass = executable_graph.passes()[handle.index];
            if (pass.kind == RenderGraphPassKind::Bloom) {
                custom_graph.before_bloom_presentation_output = logical(pass.input);
            } else if (pass.kind == RenderGraphPassKind::ToneMapping) {
                if (!custom_graph.before_bloom_presentation_output) {
                    custom_graph.before_bloom_presentation_output = logical(pass.input);
                }
                custom_graph.after_bloom_presentation_output = logical(pass.input);
            }
        }

        vector<RendererApi::CustomPostProcessEffect> custom_effects;

        RendererApi::RenderFrameDesc desc{};
        desc.surface = frame.surface;
        desc.offscreen_target = RendererApi::OffscreenRenderTargetHandle{
            .value = executable_graph.selected_render_target().value,
        };
        desc.frame = frame.frame;
        desc.view.camera = frame.camera;
        desc.view.lighting = frame.lighting;
        desc.view.deferred_formats = frame.deferred_formats;
        desc.view.render_graph = RendererApi::RenderGraphSettings{
            .render_scene = has_scene && graph.scene.enabled,
            .spectral_path_tracing = RendererApi::SpectralPathTracingSettings{
                .mode = lower_scene_integrator(graph.scene.integrator),
                .samples_per_pixel = graph.scene.path_samples_per_pixel,
                .max_bounces = graph.scene.path_max_bounces,
                .russian_roulette_start_bounce = graph.scene.path_russian_roulette_start_bounce,
                .photon_count = graph.scene.caustic_photon_count,
                .caustic_gather_radius = graph.scene.caustic_gather_radius,
                .wavelength_min_nm = graph.scene.wavelength_min_nm,
                .wavelength_max_nm = graph.scene.wavelength_max_nm,
            },
            .surfel_gi = RendererApi::SurfelGiSettings{
                .enabled = has_scene && graph.surfel_gi.enabled && capabilities().raytracing,
                .surfel_radius_near = graph.surfel_gi.surfel_radius_near,
                .surfel_radius_far = graph.surfel_gi.surfel_radius_far,
                .cascade_count = graph.surfel_gi.cascade_count,
                .cascade_distances = {
                    graph.surfel_gi.cascade_distances[0], graph.surfel_gi.cascade_distances[1],
                    graph.surfel_gi.cascade_distances[2], graph.surfel_gi.cascade_distances[3],
                },
                .hash_grid_bucket_count = graph.surfel_gi.hash_grid_bucket_count,
                .max_surfels = graph.surfel_gi.max_surfels,
                .max_surfels_spawned_per_frame = graph.surfel_gi.max_surfels_spawned_per_frame,
                .screen_coverage_target = graph.surfel_gi.screen_coverage_target,
                .rays_per_surfel_per_frame = graph.surfel_gi.rays_per_surfel_per_frame,
                .max_ray_distance = graph.surfel_gi.max_ray_distance,
                .temporal_accumulation_alpha = graph.surfel_gi.temporal_accumulation_alpha,
                .irradiance_sample_radius_multiplier = graph.surfel_gi.irradiance_sample_radius_multiplier,
                .intensity = graph.surfel_gi.intensity,
                .quality = static_cast<u32>(graph.surfel_gi.quality),
                .show_debug_surfels = graph.surfel_gi.show_debug_surfels,
            },
            .motion_blur = RendererApi::MotionBlurSettings{
                .enabled = has_scene && graph.motion_blur.enabled,
                .intensity = graph.motion_blur.intensity,
                .shutter_angle_degrees = graph.motion_blur.shutter_angle_degrees,
                .tile_size_px = graph.motion_blur.tile_size_px,
                .sample_count = graph.motion_blur.sample_count,
                .max_blur_radius_px = graph.motion_blur.max_blur_radius_px,
                .background_foreground_weight_bias = graph.motion_blur.background_foreground_weight_bias,
                .camera_motion_only = graph.motion_blur.camera_motion_only,
            },
            .shadows = has_scene && graph.shadows.enabled,
            .ambient_occlusion = has_scene && graph.ambient_occlusion.enabled,
            .bloom = has_bloom && graph.bloom.enabled,
            .tone_mapping = has_tone_mapping && graph.tone_mapping.enabled,
            .debug_overlay = has_debug_overlay && graph.debug_overlay.enabled,
            .draw_overlay_text = graph.debug_overlay.draw_text,
            .wait_for_completion = graph.execution_mode == RenderGraphExecutionMode::WaitForCompletion,
            .resolution_scale = graph.resolution_scale,
            .background_color = graph.scene.background_color.value_or(glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}),
            .background_intensity = graph.scene.background_intensity,
            .shadow_atlas_size = graph.shadows.atlas_size,
            .shadow_cascade_count = graph.shadows.cascade_count,
            .shadow_max_distance = graph.shadows.max_distance,
            .shadow_cascade_split_lambda = graph.shadows.cascade_split_lambda,
            .shadow_cascade_blend = graph.shadows.cascade_blend,
            .shadow_depth_bias = graph.shadows.depth_bias,
            .shadow_slope_bias = graph.shadows.slope_bias,
            .shadow_normal_bias = graph.shadows.normal_bias,
            .max_shadowed_spot_lights = graph.shadows.max_shadowed_spot_lights,
            .max_shadowed_point_lights = graph.shadows.max_shadowed_point_lights,
            .shadow_contact_hardening = graph.shadows.contact_hardening,
            .contact_shadows = graph.shadows.contact_shadows,
            .contact_shadow_distance = graph.shadows.contact_shadow_distance,
            .contact_shadow_thickness = graph.shadows.contact_shadow_thickness,
            .contact_shadow_steps = graph.shadows.contact_shadow_steps,
            .gtao_radius = graph.ambient_occlusion.radius,
            .gtao_falloff = graph.ambient_occlusion.falloff,
            .gtao_thickness = graph.ambient_occlusion.thickness,
            .gtao_intensity = graph.ambient_occlusion.intensity,
            .gtao_quality = static_cast<u32>(graph.ambient_occlusion.quality),
            .msaa_samples = has_anti_aliasing ? graph.anti_aliasing.msaa_samples : 1u,
            .post_process_aa = has_anti_aliasing ? static_cast<u32>(graph.anti_aliasing.post_process) : static_cast<u32>(PostProcessAntiAliasing::None),
            .aa_subpixel_quality = graph.anti_aliasing.subpixel_quality,
            .aa_edge_threshold = graph.anti_aliasing.edge_threshold,
            .bloom_threshold = graph.bloom.threshold,
            .bloom_soft_knee = graph.bloom.soft_knee,
            .bloom_intensity = graph.bloom.intensity,
            .bloom_scatter = graph.bloom.scatter,
            .bloom_downsample_ratio = graph.bloom.downsample_ratio,
            .bloom_max_levels = graph.bloom.max_levels,
            .tone_mapping_operator = lower_tone_mapping(graph.tone_mapping.operation),
            .tone_mapping_exposure = graph.tone_mapping.exposure,
            .tone_mapping_white_point = graph.tone_mapping.white_point,
            .tone_mapping_saturation = graph.tone_mapping.saturation,
            .tone_mapping_hdr_paper_white_nits = graph.tone_mapping.hdr_paper_white_nits,
            .tone_mapping_hdr_peak_nits = graph.tone_mapping.hdr_peak_nits,
            .agx_look = lower_agx_look(graph.tone_mapping.agx.look),
            .hermite_toe_strength = graph.tone_mapping.hermite_spline.toe_strength,
            .hermite_toe_length = graph.tone_mapping.hermite_spline.toe_length,
            .hermite_shoulder_strength = graph.tone_mapping.hermite_spline.shoulder_strength,
            .hermite_shoulder_length = graph.tone_mapping.hermite_spline.shoulder_length,
            .hermite_shoulder_angle = graph.tone_mapping.hermite_spline.shoulder_angle,
            .psychov_highlights = graph.tone_mapping.psycho_v.highlights,
            .psychov_shadows = graph.tone_mapping.psycho_v.shadows,
            .psychov_contrast = graph.tone_mapping.psycho_v.contrast,
            .psychov_purity_scale = graph.tone_mapping.psycho_v.purity_scale,
            .psychov_gamut_compression = graph.tone_mapping.psycho_v.gamut_compression,
            .psychov_gamut_compression_use_bt2020 = graph.tone_mapping.psycho_v.gamut_compression_use_bt2020,
            .psychov_compression = graph.tone_mapping.psycho_v.compression,
            .psychov_adapted_gray_bt709 = graph.tone_mapping.psycho_v.adapted_gray_bt709,
            .psychov_background_gray_bt709 = graph.tone_mapping.psycho_v.background_gray_bt709,
            .custom_post_processes = std::move(custom_effects),
            .custom_graph = std::move(custom_graph),
            .ui_overlay = frame.ui_overlay,
        };
        desc.view.visibility_mask = frame.visibility_mask;
        if (frame.renderables) {
            desc.view.renderables = span<const RendererApi::SceneRenderable>{
                frame.renderables->data(),
                frame.renderables->size()};
        }
        if (frame.gizmo_renderables) {
            desc.view.gizmo_renderables = span<const RendererApi::SceneRenderable>{
                frame.gizmo_renderables->data(),
                frame.gizmo_renderables->size()};
        }
        desc.view.debug_label = frame.debug_label;
        return renderer_.render_frame(desc);
    }

    /// Waits for idle to complete.
    ///
    /// @return Returns the current wait idle value.
    /// @note This function does not throw exceptions.
    void Engine::wait_idle() noexcept {
        renderer_.wait_idle();
    }

    /// Returns the current GPU info.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    optional<Core::GpuInfo> Engine::gpu_info() const {
        return renderer_.gpu_info();
    }

    /// Returns the current or globally available GPU inventory value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const RHI::GpuInventory &Engine::gpu_inventory() const noexcept {
        return gpu_inventory_;
    }

    /// Returns the current or globally available graphics backend value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    Core::EngineBackend *Engine::graphics_backend() noexcept {
        return renderer_.graphics_backend();
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    RHI::RhiDevice *Engine::rhi_device() noexcept {
        return renderer_.rhi_device();
    }

} // namespace SFT::Engine
