#include <RuntimeDemoGameLogic/RuntimeDemoGameLogic.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

using std::span;

namespace RendererApi = SFT::Renderer;

namespace SFT::Runtime {

    namespace {
        struct ThresholdConstants {
            f32 threshold = 1.0f;
            f32 soft_knee = 0.5f;
        };
        static_assert(sizeof(ThresholdConstants) == 8);
    } // namespace

    /// Runs the requested work.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    RuntimeDemoGameLogic::RuntimeDemoGameLogic() {
        camera_ = Engine::Camera::perspective(55.0f, 16.0f / 9.0f, 0.05f, 200.0f);

        camera_.set_position({-10.5f, 2.2f, 0.0f});
        camera_.look_at({0.0f, 1.0f, 0.0f});


        render_graph_ = Engine::RenderGraph::standard();
        render_graph_
            .set_resolution_scale(1.0f)
            .set_tone_mapping(Engine::ToneMappingOperator::PsychoV, 0.55f)
            .configure_bloom([](Engine::BloomSettings &bloom) { bloom.threshold = 3.20f; })
            .enable(Engine::RenderFeature::DebugOverlay);
        render_graph_.scene().integrator = Engine::SceneIntegrator::RasterDeferred;
        render_graph_.scene().path_samples_per_pixel = 1;
        render_graph_.scene().path_max_bounces = 4;
        render_graph_.scene().path_russian_roulette_start_bounce = 3;
        render_graph_.scene().caustic_photon_count = 262144;
        render_graph_.scene().caustic_gather_radius = 0.075f;
        render_graph_.anti_aliasing().msaa_samples = 4;
        render_graph_.anti_aliasing().post_process = Engine::PostProcessAntiAliasing::None;
    }

    /// Handles the on engine initialized callback and updates the associated platform state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Engine::GameLogicResult RuntimeDemoGameLogic::on_engine_initialized(Engine::Engine &engine) {
        engine_config_ = engine.config();
        if (Engine::AssetResult content = create_demo_content(engine); !content) {
            return std::unexpected(Engine::GameLogicError{.message = content.error().message});
        }
        configure_render_extraction(engine);
        configure_event_systems(engine);
        spawn_demo_entities(engine);
        Foundation::log_info(
            "Runtime demo baseline: native-resolution raster deferred, 4x spatial/deferred MSAA, "
            "no temporal AA, no post-process AA, no upscaling, and no frame generation.");
        Foundation::log_info(
            "Press P to toggle the spectral path-tracing comparison {}; press M to cycle raster MSAA.",
            engine.capabilities().raytracing ? "(available)" : "(unavailable on this device)");
        return {};
    }


    /// Creates a demo content from the supplied parameters.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Engine::AssetResult RuntimeDemoGameLogic::create_demo_content(Engine::Engine &engine) {
        Engine::AssetManager &assets = engine.assets();
        auto shader = assets.load_shader(Engine::ShaderAssetDesc{
            .source = "Shaders/gbuffer_geometry.slang",
            .label = UString{"gltf gbuffer shader"_ustr},
            .depth_only_fragment_entry_point = UString{"depthOnlyMain"_ustr},
        });
        if (!shader) {
            return std::unexpected(shader.error());
        }
        gltf_shader_ = *shader;

        auto floor_model = assets.create_model(Engine::ModelAssetDesc{
            .label = UString{"runtime reference floor"_ustr},
            .primitives = {
                Engine::ModelPrimitiveDesc{
                    .mesh = RendererApi::Mesh::plane(
                        {.width = 18.0f, .depth = 18.0f, .width_segments = 1, .depth_segments = 1},
                        "runtime reference floor"),
                    .shader = gltf_shader_,
                },
            },
        });
        if (!floor_model) {
            return std::unexpected(floor_model.error());
        }
        reference_floor_model_ = *floor_model;

        const auto configure_reference_material = [&](Engine::Asset model,
                                                      const glm::vec4 &base_color,
                                                      f32 metallic,
                                                      f32 roughness,
                                                      f32 transmission,
                                                      f32 dispersion,
                                                      f32 absorption) -> Engine::AssetResult {
            Engine::AssetResult result = assets.set_model_vec4(model, 0, "base_color_factor", base_color);
            if (result) result = assets.set_model_float(model, 0, "metallic_factor", metallic);
            if (result) result = assets.set_model_float(model, 0, "roughness_factor", roughness);
            if (result) result = assets.set_model_float(model, 0, "specular_factor", 1.0f);
            if (result) result = assets.set_model_float(model, 0, "ior", 1.5f);
            if (result) result = assets.set_model_float(model, 0, "transmission_factor", transmission);
            if (result) result = assets.set_model_float(model, 0, "dispersion_cauchy_b", dispersion);
            if (result) result = assets.set_model_float(model, 0, "absorption_coefficient", absorption);
            if (result) result = assets.set_model_float(model, 0, "alpha_cutoff", 0.0f);
            if (result) result = assets.set_model_float(model, 0, "occlusion_strength", 1.0f);
            if (result) result = assets.set_model_vec4(model, 0, "emissive_factor", glm::vec4{0.0f});
            if (result) result = assets.set_model_float(model, 0, "emissive_strength", 1.0f);
            return result;
        };

        if (Engine::AssetResult configured = configure_reference_material(
                reference_floor_model_, glm::vec4{0.62f, 0.64f, 0.68f, 1.0f},
                0.0f, 0.82f, 0.0f, 0.0f, 0.0f);
            !configured) {
            return configured;
        }

#ifdef STURDY_GLTF_SAMPLE_ASSETS_DIR
        auto gltf = Engine::import_gltf(
            assets,
            std::filesystem::path{STURDY_GLTF_SAMPLE_ASSETS_DIR} / "Models" / "Sponza" / "glTF" / "Sponza.gltf",
            gltf_shader_);
        if (!gltf) {
            return std::unexpected(gltf.error());
        }
        gltf_instances_ = std::move(gltf->instances);
        gltf_lights_ = std::move(gltf->lights);
#endif

        return {};
    }

    /// Configures render extraction using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void RuntimeDemoGameLogic::configure_render_extraction(Engine::Engine &engine) {
        engine.ecs_world().bind_resource(engine.render_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::ModelRenderer &model_renderer,
               Ecs::WriteResource<Engine::RenderFrameRequests> render) noexcept {
                render->submit(entity, transform, model_renderer);
            });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::LightGizmoRenderer &gizmo_renderer,
               Ecs::WriteResource<Engine::RenderFrameRequests> render) noexcept {
                render->submit_gizmo(entity, transform, gizmo_renderer);
            });

        engine.ecs_world().bind_resource(engine.light_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::DirectionalLightRenderer &light,
               Ecs::WriteResource<Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::SpotLightRenderer &light,
               Ecs::WriteResource<Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity,
               const Engine::WorldTransform &transform,
               const Engine::PointLightRenderer &light,
               Ecs::WriteResource<Engine::LightFrameRequests> lights) noexcept {
                lights->submit(entity, transform, light);
            });
    }

    /// Configures event systems using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void RuntimeDemoGameLogic::configure_event_systems(Engine::Engine &engine) {
        bloom_threshold_events_.build(engine.ecs_world(), engine.update_schedule());
        bloom_controls_entity_ = engine.ecs_world().spawn(
            BloomKeyboardControls{.threshold_step = 0.05f},
            BloomTuningState{.threshold = render_graph_.bloom().threshold});

        hdr_controls_entity_ = engine.ecs_world().spawn(HdrToggleState{});
        engine.update_schedule().add_system(
            [](Ecs::Entity, HdrToggleState &state, Ecs::EventReader<Engine::KeyboardEvent> keyboard) noexcept {
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    if (!event.pressed() || event.repeat) continue;
                    if (event.key == 'h' || event.key == 'H') {
                        state.toggle_requested = true;
                    } else if (event.key == 'j' || event.key == 'J') {
                        state.cycle_color_space_requested = true;
                    } else if (event.key == 'k' || event.key == 'K') {
                        state.refresh_metadata_requested = true;
                    }
                }
            });

        engine.update_schedule().add_system(
            [](Ecs::Entity,
               const BloomKeyboardControls &controls,
               BloomTuningState &state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard,
               Ecs::EventWriter<BloomThresholdChanged> changed_events) noexcept {
                constexpr i32 key_plus = '+';
                constexpr i32 key_equal = '=';
                constexpr i32 key_minus = '-';
                constexpr i32 glfw_keypad_add = 334;
                constexpr i32 glfw_keypad_subtract = 333;
                constexpr i32 sdl_keypad_add = 1073741911;
                constexpr i32 sdl_keypad_subtract = 1073741910;

                bool changed = false;
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    if (!event.pressed()) continue;
                    if (event.key == key_plus || event.key == key_equal ||
                        event.key == glfw_keypad_add || event.key == sdl_keypad_add) {
                        state.threshold += controls.threshold_step;
                        changed = true;
                    } else if (event.key == key_minus || event.key == glfw_keypad_subtract ||
                               event.key == sdl_keypad_subtract) {
                        state.threshold = std::max(0.0f, state.threshold - controls.threshold_step);
                        changed = true;
                    } else if ((event.key == 'b' || event.key == 'B') && !event.repeat) {
                        state.threshold_view = !state.threshold_view;
                        changed = true;
                    }
                }
                if (changed) {
                    changed_events.send(BloomThresholdChanged{
                        .threshold = state.threshold,
                        .threshold_view = state.threshold_view,
                    });
                }
            });


        engine.update_schedule().add_system(
            [](Ecs::EventReader<BloomThresholdChanged> changes) noexcept {
                for (const BloomThresholdChanged &change : changes.read()) {
                    Foundation::log_info("Bloom threshold: {:.2f}; threshold view: {}",
                                         change.threshold, change.threshold_view ? "on" : "off");
                }
            });


        runtime_rendering_events_.build(engine.ecs_world(), engine.update_schedule());
        runtime_rendering_entity_ = engine.ecs_world().spawn(
            RuntimeRenderingState{
                .mode = RuntimeRenderingMode::NativeRaster,
                .raster_msaa_samples = render_graph_.anti_aliasing().msaa_samples,
                .spectral_path_tracing_available = static_cast<bool>(engine.capabilities().raytracing),
            });
        engine.update_schedule().add_system(
            [](Ecs::Entity,
               RuntimeRenderingState &state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard,
               Ecs::EventWriter<RuntimeRenderingSettingsChanged> changed_events) noexcept {
                bool changed = false;
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    if (!event.pressed() || event.repeat) continue;
                    if (event.key == 'p' || event.key == 'P') {
                        if (!state.spectral_path_tracing_available) {
                            Foundation::log_warn(
                                "Spectral path tracing is unavailable because this device did not negotiate ray tracing.");
                            continue;
                        }
                        state.mode = state.mode == RuntimeRenderingMode::NativeRaster
                                         ? RuntimeRenderingMode::SpectralPathTracing
                                         : RuntimeRenderingMode::NativeRaster;
                        changed = true;
                    } else if (event.key == 'm' || event.key == 'M') {
                        switch (state.raster_msaa_samples) {
                            case 1: state.raster_msaa_samples = 2; break;
                            case 2: state.raster_msaa_samples = 4; break;
                            case 4: state.raster_msaa_samples = 8; break;
                            default: state.raster_msaa_samples = 1; break;
                        }
                        changed = true;
                    }
                }
                if (changed) {
                    changed_events.send(RuntimeRenderingSettingsChanged{
                        .mode = state.mode,
                        .raster_msaa_samples = state.raster_msaa_samples,
                    });
                }
            });
        engine.update_schedule().add_system(
            [](Ecs::EventReader<RuntimeRenderingSettingsChanged> changes) noexcept {
                for (const RuntimeRenderingSettingsChanged &change : changes.read()) {
                    if (change.mode == RuntimeRenderingMode::NativeRaster) {
                        Foundation::log_info(
                            "Runtime renderer: native raster deferred, {}x MSAA, no post-process AA.",
                            change.raster_msaa_samples);
                    } else {
                        Foundation::log_info(
                            "Runtime renderer: spectral path-tracing comparison (raster MSAA disabled; "
                            "native raster comparison remains set to {}x MSAA).",
                            change.raster_msaa_samples);
                    }
                }
            });


        spectral_path_tracing_events_.build(engine.ecs_world(), engine.update_schedule());
        spectral_path_tracing_controls_entity_ = engine.ecs_world().spawn(
            SpectralPathTracingKeyboardControls{},
            SpectralPathTracingTuningState{
                .samples_per_pixel = render_graph_.scene().path_samples_per_pixel,
                .max_bounces = render_graph_.scene().path_max_bounces,
            });
        engine.update_schedule().add_system(
            [](Ecs::Entity,
               const SpectralPathTracingKeyboardControls &controls,
               SpectralPathTracingTuningState &state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard,
               Ecs::EventWriter<SpectralPathTracingSettingsChanged> changed_events) noexcept {
                bool changed = false;
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    if (!event.pressed()) continue;
                    if (event.key == '[') {
                        state.max_bounces = std::max(1u, state.max_bounces - controls.bounce_step);
                        changed = true;
                    } else if (event.key == ']') {
                        state.max_bounces = std::min(64u, state.max_bounces + controls.bounce_step);
                        changed = true;
                    } else if (event.key == ',') {
                        state.samples_per_pixel = std::max(1u, state.samples_per_pixel - controls.sample_step);
                        changed = true;
                    } else if (event.key == '.') {
                        state.samples_per_pixel = std::min(64u, state.samples_per_pixel + controls.sample_step);
                        changed = true;
                    }
                }
                if (changed) {
                    changed_events.send(SpectralPathTracingSettingsChanged{
                        .samples_per_pixel = state.samples_per_pixel,
                        .max_bounces = state.max_bounces,
                    });
                }
            });
        engine.update_schedule().add_system(
            [](Ecs::EventReader<SpectralPathTracingSettingsChanged> changes) noexcept {
                for (const SpectralPathTracingSettingsChanged &change : changes.read()) {
                    Foundation::log_info("Path tracing: {} spp, {} bounces",
                                         change.samples_per_pixel, change.max_bounces);
                }
            });


        tweak_panel_entity_ = engine.ecs_world().spawn(
            TweakPanelState{},
            SurfelGiTuningState{},
            MotionBlurTuningState{});
        engine.update_schedule().add_system(
            [](Ecs::Entity,
               TweakPanelState &panel,
               SurfelGiTuningState &gi,
               MotionBlurTuningState &blur,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard) noexcept {
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    if (!event.pressed() || event.repeat) continue;
                    if (event.key == 'u' || event.key == 'U') {
                        panel.visible = !panel.visible;
                    } else if (event.key == 'g' || event.key == 'G') {
                        gi.enabled = !gi.enabled;
                    } else if (event.key == 'v' || event.key == 'V') {
                        blur.enabled = !blur.enabled;
                    }
                }
            });

        const glm::vec3 initial_euler = camera_.euler_degrees();
        camera_control_entity_ = engine.ecs_world().spawn(FlyCameraState{
            .yaw_degrees = initial_euler.y,
            .pitch_degrees = initial_euler.x,
        });

        engine.update_schedule().add_system(
            [](Ecs::Entity,
               FlyCameraState &state,
               Ecs::EventReader<Engine::KeyboardEvent> keyboard,
               Ecs::EventReader<Engine::MouseMoveEvent> mouse) noexcept {
                for (const Engine::KeyboardEvent &event : keyboard.read()) {
                    const bool down = event.pressed();
                    switch (event.key) {
                        case 'w': case 'W': state.move_forward = down; break;
                        case 's': case 'S': state.move_backward = down; break;
                        case 'a': case 'A': state.move_left = down; break;
                        case 'd': case 'D': state.move_right = down; break;
                        case 'e': case 'E': state.move_up = down; break;
                        case 'q': case 'Q': state.move_down = down; break;
                        default: break;
                    }
                }


                constexpr u32 right_mouse_button_mask = 0x4u;
                for (const Engine::MouseMoveEvent &event : mouse.read()) {
                    if ((event.mouse.buttons & right_mouse_button_mask) != 0) {
                        state.look_delta_x += event.mouse.delta_x;
                        state.look_delta_y += event.mouse.delta_y;
                    }
                }
            });
    }

    /// Spawns demo entities.
    ///
    /// @param engine `engine` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void RuntimeDemoGameLogic::spawn_demo_entities(Engine::Engine &engine) {
#ifdef STURDY_GLTF_SAMPLE_ASSETS_DIR
        for (const Engine::GltfNodeInstance &instance : gltf_instances_) {
            if (instance.model) {
                (void)engine.ecs_world().spawn(
                    Engine::WorldTransform{.value = instance.world_transform},
                    Engine::ModelRenderer{.model = instance.model});
            }
        }
        for (const Engine::GltfLightInstance &light : gltf_lights_) {
            const Engine::WorldTransform transform{.value = light.world_transform};
            switch (light.kind) {
                case Engine::GltfLightKind::Directional:
                    (void)engine.ecs_world().spawn(
                        transform, Engine::DirectionalLightRenderer{.radiance = light.radiance});
                    break;
                case Engine::GltfLightKind::Point:
                    (void)engine.ecs_world().spawn(
                        transform, Engine::PointLightRenderer{.radiance = light.radiance, .range = light.range});
                    break;
                case Engine::GltfLightKind::Spot:
                    (void)engine.ecs_world().spawn(
                        transform, Engine::SpotLightRenderer{
                            .radiance = light.radiance,
                            .range = light.range,
                            .inner_cone_cos = light.inner_cone_cos,
                            .outer_cone_cos = light.outer_cone_cos,
                        });
                    break;
            }
        }
#else
        (void)engine.ecs_world().spawn(
            Engine::WorldTransform{.value = glm::mat4{1.0f}},
            Engine::ModelRenderer{.model = reference_floor_model_});
#endif



        {
            const glm::vec3 sun_direction = glm::normalize(glm::vec3{0.18f, -1.0f, 0.12f});
            glm::mat4 sun_transform =
                glm::inverse(glm::lookAtRH(glm::vec3{0.0f}, sun_direction, glm::vec3{0.0f, 1.0f, 0.0f}));
            sun_transform = sun_transform * glm::rotate(
                glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = sun_transform},
                Engine::DirectionalLightRenderer{.radiance = {3.0f, 2.8f, 2.5f}});
        }

    }

    namespace {
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
    } // namespace

    /// Performs the handle HDR controls operation for `Runtime` using the supplied arguments.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void RuntimeDemoGameLogic::handle_hdr_controls(Engine::Engine &engine, Core::RenderSurfaceHandle surface) {
        auto state = engine.ecs_world().get_component<HdrToggleState>(hdr_controls_entity_);
        if (!state) {
            return;
        }

        if (state->toggle_requested) {
            state->toggle_requested = false;


            if (const auto capability = engine.query_hdr_capabilities(surface)) {
                const RHI::SurfaceHdrCapabilities &caps = capability->capabilities;
                Foundation::log_info(
                    "HDR capability for this window's current display: supported={} enabled_by_os={} "
                    "sdr_white={:.0f} nits edr_headroom={:.2f}x supported_modes={} ({})",
                    caps.hdr_supported, caps.hdr_enabled_by_os, caps.sdr_white_nits, caps.edr_headroom,
                    caps.supported_modes.size(), capability->message.message);
            } else {
                Foundation::log_warn("HDR capability query failed: {}", capability.error().message);
            }

            Engine::EngineConfig new_config = engine_config_;
            new_config.features.presentation = engine.presentation_settings(surface);
            new_config.features.presentation.hdr_enabled = !new_config.features.presentation.hdr_enabled;
            if (const auto applied = engine.apply_runtime_settings(surface, new_config)) {
                engine_config_ = new_config;
                Foundation::log_info("HDR {} — {}",
                                     new_config.features.presentation.hdr_enabled ? "enabled" : "disabled",
                                     applied->message);
                if (RHI::RhiDevice *device = engine.rhi_device(); device != nullptr) {
                    engine.ui_context().destroy(*device);
                }
            } else {
                Foundation::log_warn("Failed to toggle HDR: {}", applied.error().message);
            }
        }

        if (state->cycle_color_space_requested) {
            state->cycle_color_space_requested = false;


            Engine::EngineConfig new_config = engine_config_;
            new_config.features.presentation = engine.presentation_settings(surface);
            const auto current = static_cast<u8>(new_config.features.presentation.hdr_color_space);
            constexpr u8 mode_count = 4;
            new_config.features.presentation.hdr_color_space =
                static_cast<Core::HdrColorSpaceMode>((current + 1) % mode_count);
            if (const auto applied = engine.apply_runtime_settings(surface, new_config)) {
                engine_config_ = new_config;
                Foundation::log_info("HDR color space set to {} — {}",
                                     hdr_color_space_name(new_config.features.presentation.hdr_color_space),
                                     applied->message);
            } else {
                Foundation::log_warn("Failed to switch HDR color space to {}: {}",
                                     hdr_color_space_name(new_config.features.presentation.hdr_color_space),
                                     applied.error().message);
            }
        }

        if (state->refresh_metadata_requested) {
            state->refresh_metadata_requested = false;


            constexpr RHI::HdrContentLightLevelUpdate demo_update{
                .max_content_light_level_nits = 600.0f,
                .max_frame_average_light_level_nits = 150.0f,
            };
            if (const auto updated = engine.update_hdr_content_light_level(surface, demo_update)) {
                Foundation::log_info(
                    "HDR content-light-level metadata refreshed (demo values: MaxCLL={:.0f} nits, MaxFALL={:.0f} nits).",
                    demo_update.max_content_light_level_nits, demo_update.max_frame_average_light_level_nits);
            } else {
                Foundation::log_warn("Failed to refresh HDR content-light-level metadata: {}", updated.error().message);
            }
        }
    }

    namespace {
        constexpr UI::FontId kTweakPanelFontId = 1;
        constexpr UI::Color kTweakPanelBackground{0.06, 0.07, 0.09, 0.88};
        constexpr UI::Color kTweakPanelOutline{0.16, 0.18, 0.24, 1.0};
        constexpr UI::Color kTweakPanelTextPrimary{0.93, 0.95, 1.0, 1.0};
        constexpr UI::Color kTweakPanelTextSecondary{0.60, 0.64, 0.76, 1.0};
        constexpr UI::Color kTweakPanelAccent{0.39, 0.57, 1.0, 1.0};

        /// Draws text using the current rendering state.
        void draw_panel_text(UI::Context &ctx, std::string_view content, UI::Color color, u16 size) {
            const UI::TextStyle style{.color = color, .font_id = kTweakPanelFontId, .font_size = size};
            ctx.text(ustr{content}, style);
        }

        /// Returns the current or globally available tweak panel toggle style value.
        [[nodiscard]] UI::ToggleStyle tweak_panel_toggle_style() {
            return UI::ToggleStyle{
                .idle = UI::Color{0.12, 0.14, 0.19, 1.0},
                .hovered = UI::Color{0.19, 0.22, 0.30, 1.0},
                .checked = kTweakPanelAccent,
                .disabled = UI::Color{0.10, 0.11, 0.13, 0.55},
                .mark_color = UI::Color{0.99, 0.99, 1.0, 1.0},
                .transition_seconds = 0.16f,
            };
        }

        /// Returns the current or globally available tweak panel slider style value.
        [[nodiscard]] UI::SliderStyle tweak_panel_slider_style() {
            return UI::SliderStyle{
                .track = UI::Color{0.12, 0.13, 0.18, 1.0},
                .fill = kTweakPanelAccent,
                .thumb = kTweakPanelTextPrimary,
                .thumb_hovered = UI::Color{1.0, 1.0, 1.0, 1.0},
                .thumb_dragging = kTweakPanelAccent,
                .track_thickness = 6.0f,
                .thumb_size = 16.0f,
            };
        }

        /// Returns the current or globally available tweak panel dropdown style value.
        [[nodiscard]] UI::DropdownStyle tweak_panel_dropdown_style() {
            UI::DropdownStyle style{};
            style.trigger = UI::ButtonStyle{
                .idle = UI::Color{0.12, 0.14, 0.19, 1.0},
                .hovered = UI::Color{0.19, 0.22, 0.30, 1.0},
                .pressed = UI::Color{0.09, 0.11, 0.17, 1.0},
                .disabled = UI::Color{0.09, 0.10, 0.12, 0.6},
                .corner_radius = UI::CornerRadius::all(8.0f),
                .border = UI::BorderStyle{.color = kTweakPanelOutline, .width = UI::BorderWidth::all(1)},
            };
            style.list_background = UI::Color{0.09, 0.10, 0.14, 0.98};
            style.option_hovered = UI::Color{kTweakPanelAccent.r, kTweakPanelAccent.g, kTweakPanelAccent.b, 0.22};
            style.arrow_font_id = kTweakPanelFontId;
            return style;
        }

        UI::DropdownOption tweak_panel_dropdown_option(const char *label) {
            return UI::DropdownOption{.build = [label](UI::Context &option_ctx) {
                draw_panel_text(option_ctx, label, kTweakPanelTextPrimary, 12);
            }};
        }

        /// Draws a "name: value" label above a slider so each control is identifiable on its own.
        void draw_slider_label(UI::Context &ctx, const char *name, f64 value) {
            draw_panel_text(ctx, std::format("{}: {:.2f}", name, value), kTweakPanelTextPrimary, 12);
        }
    } // namespace

    /// Builds the top-right tweak panel overlay (surfel GI, motion blur, bloom, tone mapping, shadows).
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns the current build tweak panel overlay value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Renderer::UiOverlayHooks RuntimeDemoGameLogic::build_tweak_panel_overlay(
        Engine::Engine &engine, Core::RenderSurfaceHandle, const Core::FrameInput &frame) {
        auto panel_state = engine.ecs_world().get_component<TweakPanelState>(tweak_panel_entity_);
        if (!panel_state || !panel_state->visible) {
            return {};
        }

        RHI::RhiDevice *device = engine.rhi_device();
        if (device == nullptr) {
            return {};
        }
        const RHI::Format color_format = engine_config_.features.presentation.hdr_enabled
                                              ? RHI::Format::RGBA16Float
                                              : RHI::Format::BGRA8UnormSrgb;
        if (!engine.ui_context().ensure_ready(*device, color_format)) {
            return {};
        }
        if (!tweak_panel_font_registered_) {
            const std::optional<std::string> font_bytes =
                Foundation::read_file_to_string("Fonts/MapleMono-NF-Regular.ttf");
            if (font_bytes) {
                const std::span<const char> chars{font_bytes->data(), font_bytes->size()};
                if (auto loaded = Text::Font::load(std::as_bytes(chars))) {
                    tweak_panel_font_ = std::move(*loaded);
                    engine.ui_context().context().register_font(kTweakPanelFontId, tweak_panel_font_);
                    tweak_panel_font_registered_ = true;
                } else {
                    Foundation::log_warn("Runtime tweak panel: failed to load font: {}", loaded.error().message);
                }
            } else {
                Foundation::log_warn("Runtime tweak panel: could not read Fonts/MapleMono-NF-Regular.ttf");
            }
        }

        const glm::vec2 viewport{
            static_cast<f32>(frame.framebuffer_width), static_cast<f32>(frame.framebuffer_height)};
        engine.ui_context().begin_layout(viewport, engine.ui_pointer_state(), static_cast<f32>(frame.delta_seconds));
        UI::Context &ctx = engine.ui_context().context();

        auto gi = engine.ecs_world().get_component<SurfelGiTuningState>(tweak_panel_entity_);
        auto blur = engine.ecs_world().get_component<MotionBlurTuningState>(tweak_panel_entity_);
        auto bloom = engine.ecs_world().get_component<BloomTuningState>(bloom_controls_entity_);
        const bool raytracing_available = static_cast<bool>(engine.capabilities().raytracing);

        {
            auto root = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(viewport.x), UI::SizingAxis::fixed(viewport.y)},
                .padding = UI::Padding::all(16),
                .child_alignment = {UI::AlignX::Right, UI::AlignY::Top},
            });
            auto panel = ctx.element(UI::ElementDecl{
                .sizing = {UI::SizingAxis::fixed(300.0f), UI::SizingAxis::fit()},
                .padding = UI::Padding::all(14),
                .child_gap = 12,
                .direction = UI::LayoutDirection::TopToBottom,
                .background_color = kTweakPanelBackground,
                .corner_radius = UI::CornerRadius::all(10.0f),
                .border = UI::BorderStyle{.color = kTweakPanelOutline, .width = UI::BorderWidth::all(1)},
            });

            draw_panel_text(ctx, "Render Tweaks (U to hide)", kTweakPanelTextPrimary, 14);

            {
                draw_panel_text(ctx, "Surfel GI (G)", kTweakPanelTextSecondary, 12);
                if (gi) {
                    const bool enabled = raytracing_available && gi->enabled;
                    const UI::ToggleResult toggle_result = UI::switch_toggle(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                            .id = UString{"runtime-tweak-surfel-gi-toggle"_ustr},
                        },
                        tweak_panel_toggle_style(), surfel_gi_toggle_state_, static_cast<f32>(frame.delta_seconds),
                        enabled, raytracing_available);
                    if (toggle_result.clicked) gi->enabled = !gi->enabled;
                    if (!raytracing_available) {
                        draw_panel_text(ctx, "unavailable: no ray tracing support", kTweakPanelTextSecondary, 10);
                    }

                    draw_slider_label(ctx, "Intensity", static_cast<f64>(gi->intensity));
                    f64 intensity = static_cast<f64>(gi->intensity);
                    const UI::SliderResult intensity_result = UI::slider(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                            .id = UString{"runtime-tweak-surfel-gi-intensity"_ustr},
                        },
                        UI::SliderConfig{.min = 0.0, .max = 4.0, .step = 0.05},
                        tweak_panel_slider_style(), surfel_gi_intensity_slider_state_, intensity, UI::SliderInput{},
                        raytracing_available);
                    gi->intensity = static_cast<f32>(intensity_result.value);

                    draw_panel_text(ctx, "Quality", kTweakPanelTextPrimary, 12);
                    std::array<UI::DropdownOption, 3> quality_options{
                        tweak_panel_dropdown_option("Low"), tweak_panel_dropdown_option("Medium"),
                        tweak_panel_dropdown_option("High")};
                    usize quality_index = std::min<usize>(gi->quality, quality_options.size() - 1);
                    const UI::DropdownResult quality_result = UI::dropdown(
                        ctx,
                        UString{"runtime-tweak-surfel-gi-quality"_ustr},
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(32.0f)},
                            .padding = UI::Padding::symmetric(10, 6),
                            .id = UString{"runtime-tweak-surfel-gi-quality"_ustr},
                        },
                        tweak_panel_dropdown_style(), surfel_gi_quality_dropdown_state_,
                        static_cast<f32>(frame.delta_seconds), quality_index,
                        span<const UI::DropdownOption>{quality_options.data(), quality_options.size()},
                        raytracing_available);
                    gi->quality = static_cast<u32>(quality_result.selected_index);
                }
            }

            {
                draw_panel_text(ctx, "Motion Blur (V)", kTweakPanelTextSecondary, 12);
                if (blur) {
                    const UI::ToggleResult toggle_result = UI::switch_toggle(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                            .id = UString{"runtime-tweak-motion-blur-toggle"_ustr},
                        },
                        tweak_panel_toggle_style(), motion_blur_toggle_state_, static_cast<f32>(frame.delta_seconds),
                        blur->enabled);
                    if (toggle_result.clicked) blur->enabled = !blur->enabled;

                    draw_slider_label(ctx, "Intensity", static_cast<f64>(blur->intensity));
                    f64 intensity = static_cast<f64>(blur->intensity);
                    const UI::SliderResult intensity_result = UI::slider(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                            .id = UString{"runtime-tweak-motion-blur-intensity"_ustr},
                        },
                        UI::SliderConfig{.min = 0.0, .max = 2.0, .step = 0.05},
                        tweak_panel_slider_style(), motion_blur_intensity_slider_state_, intensity);
                    blur->intensity = static_cast<f32>(intensity_result.value);

                    draw_slider_label(ctx, "Shutter Angle", static_cast<f64>(blur->shutter_angle_degrees));
                    f64 shutter = static_cast<f64>(blur->shutter_angle_degrees);
                    const UI::SliderResult shutter_result = UI::slider(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                            .id = UString{"runtime-tweak-motion-blur-shutter"_ustr},
                        },
                        UI::SliderConfig{.min = 0.0, .max = 360.0, .step = 1.0},
                        tweak_panel_slider_style(), motion_blur_shutter_slider_state_, shutter);
                    blur->shutter_angle_degrees = static_cast<f32>(shutter_result.value);
                }
            }

            {
                draw_panel_text(ctx, "Bloom", kTweakPanelTextSecondary, 12);
                if (bloom) {
                    const UI::ToggleResult toggle_result = UI::switch_toggle(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                            .id = UString{"runtime-tweak-bloom-toggle"_ustr},
                        },
                        tweak_panel_toggle_style(), bloom_toggle_state_, static_cast<f32>(frame.delta_seconds),
                        bloom->enabled);
                    if (toggle_result.clicked) bloom->enabled = !bloom->enabled;

                    draw_slider_label(ctx, "Threshold", static_cast<f64>(bloom->threshold));
                    f64 threshold = static_cast<f64>(bloom->threshold);
                    const UI::SliderResult threshold_result = UI::slider(
                        ctx,
                        UI::ElementDecl{
                            .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                            .id = UString{"runtime-tweak-bloom-threshold"_ustr},
                        },
                        UI::SliderConfig{.min = 0.0, .max = 8.0, .step = 0.05},
                        tweak_panel_slider_style(), bloom_threshold_slider_state_, threshold);
                    bloom->threshold = static_cast<f32>(threshold_result.value);
                }
            }

            {
                draw_panel_text(ctx, "Tone Mapping", kTweakPanelTextSecondary, 12);
                draw_panel_text(ctx, "Operator", kTweakPanelTextPrimary, 12);
                constexpr std::array<Engine::ToneMappingOperator, 3> kToneMappingOperators{
                    Engine::ToneMappingOperator::Agx, Engine::ToneMappingOperator::HermiteSpline,
                    Engine::ToneMappingOperator::PsychoV};
                std::array<UI::DropdownOption, 3> operator_options{
                    tweak_panel_dropdown_option("Agx"), tweak_panel_dropdown_option("Hermite"),
                    tweak_panel_dropdown_option("PsychoV")};
                usize operator_index = 0;
                for (usize i = 0; i < kToneMappingOperators.size(); ++i) {
                    if (kToneMappingOperators[i] == render_graph_.tone_mapping().operation) {
                        operator_index = i;
                        break;
                    }
                }
                const UI::DropdownResult operator_result = UI::dropdown(
                    ctx,
                    UString{"runtime-tweak-tone-mapping-operator"_ustr},
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(32.0f)},
                        .padding = UI::Padding::symmetric(10, 6),
                        .id = UString{"runtime-tweak-tone-mapping-operator"_ustr},
                    },
                    tweak_panel_dropdown_style(), tone_mapping_operator_dropdown_state_,
                    static_cast<f32>(frame.delta_seconds), operator_index,
                    span<const UI::DropdownOption>{operator_options.data(), operator_options.size()});
                render_graph_.tone_mapping().operation = kToneMappingOperators[std::min<usize>(
                    operator_result.selected_index, kToneMappingOperators.size() - 1)];

                draw_slider_label(ctx, "Exposure", static_cast<f64>(render_graph_.tone_mapping().exposure));
                f64 exposure = static_cast<f64>(render_graph_.tone_mapping().exposure);
                const UI::SliderResult exposure_result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                        .id = UString{"runtime-tweak-tone-mapping-exposure"_ustr},
                    },
                    UI::SliderConfig{.min = 0.1, .max = 4.0, .step = 0.01},
                    tweak_panel_slider_style(), tone_mapping_exposure_slider_state_, exposure);
                render_graph_.tone_mapping().exposure = static_cast<f32>(exposure_result.value);
            }

            {
                // Exposes the knobs the XeGTAO implementation is meant to be validated against:
                // radius, thin-occluder thickness, tap budget, and the spatial denoiser on/off (the
                // last is the raw-vs-denoised A/B, since with it off the lighting pass consumes the
                // raw horizon-search buffer directly).
                draw_panel_text(ctx, "Ambient Occlusion", kTweakPanelTextSecondary, 12);
                Engine::AmbientOcclusionSettings &ao = render_graph_.ambient_occlusion();
                const UI::ToggleResult ao_toggle_result = UI::switch_toggle(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                        .id = UString{"runtime-tweak-ao-toggle"_ustr},
                    },
                    tweak_panel_toggle_style(), ambient_occlusion_toggle_state_,
                    static_cast<f32>(frame.delta_seconds), ao.enabled);
                if (ao_toggle_result.clicked) ao.enabled = !ao.enabled;

                draw_slider_label(ctx, "Radius", static_cast<f64>(ao.radius));
                f64 ao_radius = static_cast<f64>(ao.radius);
                const UI::SliderResult ao_radius_result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                        .id = UString{"runtime-tweak-ao-radius"_ustr},
                    },
                    UI::SliderConfig{.min = 0.05, .max = 4.0, .step = 0.01},
                    tweak_panel_slider_style(), ambient_occlusion_radius_slider_state_, ao_radius);
                ao.radius = static_cast<f32>(ao_radius_result.value);

                draw_slider_label(ctx, "Intensity", static_cast<f64>(ao.intensity));
                f64 ao_intensity = static_cast<f64>(ao.intensity);
                const UI::SliderResult ao_intensity_result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                        .id = UString{"runtime-tweak-ao-intensity"_ustr},
                    },
                    UI::SliderConfig{.min = 0.0, .max = 1.0, .step = 0.01},
                    tweak_panel_slider_style(), ambient_occlusion_intensity_slider_state_, ao_intensity);
                ao.intensity = static_cast<f32>(ao_intensity_result.value);

                draw_slider_label(ctx, "Thin Occluder", static_cast<f64>(ao.thin_occluder_compensation));
                f64 ao_thin = static_cast<f64>(ao.thin_occluder_compensation);
                const UI::SliderResult ao_thin_result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                        .id = UString{"runtime-tweak-ao-thin-occluder"_ustr},
                    },
                    UI::SliderConfig{.min = 0.0, .max = 0.7, .step = 0.01},
                    tweak_panel_slider_style(), ambient_occlusion_thin_occluder_slider_state_, ao_thin);
                ao.thin_occluder_compensation = static_cast<f32>(ao_thin_result.value);

                draw_panel_text(ctx, "Denoise", kTweakPanelTextPrimary, 12);
                const UI::ToggleResult ao_denoise_result = UI::switch_toggle(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                        .id = UString{"runtime-tweak-ao-denoise-toggle"_ustr},
                    },
                    tweak_panel_toggle_style(), ambient_occlusion_denoise_toggle_state_,
                    static_cast<f32>(frame.delta_seconds), ao.denoise);
                if (ao_denoise_result.clicked) ao.denoise = !ao.denoise;

                draw_panel_text(ctx, "Quality", kTweakPanelTextPrimary, 12);
                constexpr std::array<Engine::AmbientOcclusionQuality, 4> kAmbientOcclusionQualities{
                    Engine::AmbientOcclusionQuality::Low,
                    Engine::AmbientOcclusionQuality::Medium,
                    Engine::AmbientOcclusionQuality::High,
                    Engine::AmbientOcclusionQuality::Ultra,
                };
                std::array<UI::DropdownOption, 4> ao_quality_options{
                    tweak_panel_dropdown_option("Low (1x3)"),
                    tweak_panel_dropdown_option("Medium (2x4)"),
                    tweak_panel_dropdown_option("High (3x6)"),
                    tweak_panel_dropdown_option("Ultra (4x8)"),
                };
                usize ao_quality_index = std::min<usize>(
                    static_cast<usize>(ao.quality), ao_quality_options.size() - 1);
                const UI::DropdownResult ao_quality_result = UI::dropdown(
                    ctx,
                    UString{"runtime-tweak-ao-quality"_ustr},
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(32.0f)},
                        .padding = UI::Padding::symmetric(10, 6),
                        .id = UString{"runtime-tweak-ao-quality"_ustr},
                    },
                    tweak_panel_dropdown_style(), ambient_occlusion_quality_dropdown_state_,
                    static_cast<f32>(frame.delta_seconds), ao_quality_index,
                    span<const UI::DropdownOption>{ao_quality_options.data(), ao_quality_options.size()});
                ao.quality = kAmbientOcclusionQualities[std::min<usize>(
                    ao_quality_result.selected_index, kAmbientOcclusionQualities.size() - 1)];
            }

            {
                draw_panel_text(ctx, "Shadows", kTweakPanelTextSecondary, 12);
                const UI::ToggleResult toggle_result = UI::switch_toggle(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                        .id = UString{"runtime-tweak-shadows-toggle"_ustr},
                    },
                    tweak_panel_toggle_style(), shadows_toggle_state_, static_cast<f32>(frame.delta_seconds),
                    render_graph_.shadows().enabled);
                if (toggle_result.clicked) render_graph_.shadows().enabled = !render_graph_.shadows().enabled;

                draw_slider_label(ctx, "Max Distance", static_cast<f64>(render_graph_.shadows().max_distance));
                f64 max_distance = static_cast<f64>(render_graph_.shadows().max_distance);
                const UI::SliderResult distance_result = UI::slider(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(24.0f)},
                        .id = UString{"runtime-tweak-shadows-distance"_ustr},
                    },
                    UI::SliderConfig{.min = 10.0, .max = 500.0, .step = 1.0},
                    tweak_panel_slider_style(), shadows_distance_slider_state_, max_distance);
                render_graph_.shadows().max_distance = static_cast<f32>(distance_result.value);

                draw_panel_text(ctx, "Contact", kTweakPanelTextPrimary, 12);
                const UI::ToggleResult contact_toggle_result = UI::switch_toggle(
                    ctx,
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::fixed(42.0f), UI::SizingAxis::fixed(23.0f)},
                        .id = UString{"runtime-tweak-contact-shadows-toggle"_ustr},
                    },
                    tweak_panel_toggle_style(), contact_shadows_toggle_state_,
                    static_cast<f32>(frame.delta_seconds), render_graph_.shadows().contact_shadows);
                if (contact_toggle_result.clicked) {
                    render_graph_.shadows().contact_shadows = !render_graph_.shadows().contact_shadows;
                }

                draw_panel_text(ctx, "Debug View", kTweakPanelTextPrimary, 12);
                // Ordered in CSM resolve order: projection/bias inputs, depth comparison,
                // filtering, cascade composition, then the screen-space contact supplement.
                constexpr std::array<Engine::ShadowDebugView, 26> kShadowDebugViews{
                    Engine::ShadowDebugView::None,
                    Engine::ShadowDebugView::CascadeIndex,
                    Engine::ShadowDebugView::CascadeFade,
                    Engine::ShadowDebugView::ShadowTexelGrid,
                    Engine::ShadowDebugView::ShadowUv,
                    Engine::ShadowDebugView::ReceiverDepth,
                    Engine::ShadowDebugView::AtlasDepth,
                    Engine::ShadowDebugView::DepthDelta,
                    Engine::ShadowDebugView::NormalBias,
                    Engine::ShadowDebugView::ReceiverPlaneGradient,
                    Engine::ShadowDebugView::HardComparison,
                    Engine::ShadowDebugView::Pcf,
                    Engine::ShadowDebugView::DirectionalCsm,
                    Engine::ShadowDebugView::ContactShadow,
                    Engine::ShadowDebugView::CombinedSunVisibility,
                    Engine::ShadowDebugView::GbufferDepth,
                    Engine::ShadowDebugView::WorldPosition,
                    Engine::ShadowDebugView::GbufferNormal,
                    Engine::ShadowDebugView::GbufferAlbedo,
                    Engine::ShadowDebugView::GbufferRoughness,
                    Engine::ShadowDebugView::GbufferMetallic,
                    Engine::ShadowDebugView::MaterialAmbientOcclusion,
                    Engine::ShadowDebugView::AmbientLighting,
                    Engine::ShadowDebugView::SunNdotL,
                    Engine::ShadowDebugView::UnshadowedSunLighting,
                    Engine::ShadowDebugView::ScreenSpaceAmbientOcclusion,
                };
                std::array<UI::DropdownOption, 26> shadow_debug_options{
                    tweak_panel_dropdown_option("Off"),
                    tweak_panel_dropdown_option("Cascade Index"),
                    tweak_panel_dropdown_option("Cascade Fade"),
                    tweak_panel_dropdown_option("Texel Grid"),
                    tweak_panel_dropdown_option("Receiver Atlas UV"),
                    tweak_panel_dropdown_option("Receiver Depth"),
                    tweak_panel_dropdown_option("Atlas Depth"),
                    tweak_panel_dropdown_option("Depth Delta"),
                    tweak_panel_dropdown_option("Normal Bias"),
                    tweak_panel_dropdown_option("Receiver Plane Gradient"),
                    tweak_panel_dropdown_option("Hard Comparison"),
                    tweak_panel_dropdown_option("PCF"),
                    tweak_panel_dropdown_option("Directional CSM"),
                    tweak_panel_dropdown_option("Contact"),
                    tweak_panel_dropdown_option("Combined Sun Visibility"),
                    tweak_panel_dropdown_option("GBuffer Depth"),
                    tweak_panel_dropdown_option("World Position"),
                    tweak_panel_dropdown_option("GBuffer Normal"),
                    tweak_panel_dropdown_option("GBuffer Albedo"),
                    tweak_panel_dropdown_option("GBuffer Roughness"),
                    tweak_panel_dropdown_option("GBuffer Metallic"),
                    tweak_panel_dropdown_option("Material AO"),
                    tweak_panel_dropdown_option("Ambient Lighting"),
                    tweak_panel_dropdown_option("Sun N dot L"),
                    tweak_panel_dropdown_option("Unshadowed Sun Lighting"),
                    tweak_panel_dropdown_option("Screen-Space AO"),
                };
                usize shadow_debug_index = 0;
                for (usize i = 0; i < kShadowDebugViews.size(); ++i) {
                    if (kShadowDebugViews[i] == render_graph_.shadows().debug_view) {
                        shadow_debug_index = i;
                        break;
                    }
                }
                const UI::DropdownResult shadow_debug_result = UI::dropdown(
                    ctx,
                    UString{"runtime-tweak-shadow-debug-view"_ustr},
                    UI::ElementDecl{
                        .sizing = {UI::SizingAxis::grow(), UI::SizingAxis::fixed(32.0f)},
                        .padding = UI::Padding::symmetric(10, 6),
                        .id = UString{"runtime-tweak-shadow-debug-view"_ustr},
                    },
                    tweak_panel_dropdown_style(), shadow_debug_view_dropdown_state_,
                    static_cast<f32>(frame.delta_seconds), shadow_debug_index,
                    span<const UI::DropdownOption>{shadow_debug_options.data(), shadow_debug_options.size()});
                render_graph_.shadows().debug_view = kShadowDebugViews[std::min<usize>(
                    shadow_debug_result.selected_index, kShadowDebugViews.size() - 1)];
            }
        }

        auto snapshot = std::make_shared<UI::FrameSnapshot>(ctx.finish_frame(viewport));
        return engine.ui_context().build_overlay_hooks(snapshot, engine.renderer());
    }

    /// Requests render frame using the supplied arguments and current state.
    ///
    /// @param engine `engine` value used by the operation.
    /// @param surface Surface used or affected by the operation.
    /// @param frame `frame` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    std::optional<Engine::RenderFrameParameters> RuntimeDemoGameLogic::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle surface,
        const Core::FrameInput &frame) {
        handle_hdr_controls(engine, surface);

        bool threshold_view = false;
        if (auto bloom = engine.ecs_world().get_component<BloomTuningState>(bloom_controls_entity_)) {
            render_graph_.bloom().threshold = bloom->threshold;
            threshold_view = bloom->threshold_view;
            render_graph_.bloom().enabled = bloom->enabled;
        }
        if (threshold_view) {
            render_graph_.bloom().enabled = false;
        }

        if (auto rendering = engine.ecs_world().get_component<RuntimeRenderingState>(
                runtime_rendering_entity_)) {
            const bool spectral_path_tracing =
                rendering->mode == RuntimeRenderingMode::SpectralPathTracing &&
                rendering->spectral_path_tracing_available;
            render_graph_.scene().integrator = spectral_path_tracing
                                                   ? Engine::SceneIntegrator::FullPathTracing
                                                   : Engine::SceneIntegrator::RasterDeferred;
            render_graph_.anti_aliasing().msaa_samples =
                spectral_path_tracing ? 1u : rendering->raster_msaa_samples;
            render_graph_.anti_aliasing().post_process = Engine::PostProcessAntiAliasing::None;
        }

        if (auto spectral = engine.ecs_world().get_component<SpectralPathTracingTuningState>(
                spectral_path_tracing_controls_entity_)) {
            render_graph_.scene().path_samples_per_pixel = spectral->samples_per_pixel;
            render_graph_.scene().path_max_bounces = spectral->max_bounces;
        }

        if (auto gi = engine.ecs_world().get_component<SurfelGiTuningState>(tweak_panel_entity_)) {
            const bool raytracing_available = static_cast<bool>(engine.capabilities().raytracing);
            if (gi->enabled && !raytracing_available) {
                Foundation::log_warn("Surfel GI is unavailable because this device did not negotiate ray tracing.");
            }
            render_graph_.surfel_gi().enabled = gi->enabled && raytracing_available;
            render_graph_.surfel_gi().intensity = gi->intensity;
            render_graph_.surfel_gi().quality = static_cast<Engine::SurfelGiQuality>(gi->quality);
        }
        if (auto blur = engine.ecs_world().get_component<MotionBlurTuningState>(tweak_panel_entity_)) {
            render_graph_.motion_blur().enabled = blur->enabled;
            render_graph_.motion_blur().intensity = blur->intensity;
            render_graph_.motion_blur().shutter_angle_degrees = blur->shutter_angle_degrees;
        }

        Engine::RenderGraph frame_graph = render_graph_;
        if (threshold_view) {


            frame_graph = Engine::RenderGraph::empty(render_graph_.description());
            Engine::RenderGraphTextureHandle color =
                frame_graph.compose(Engine::RenderModules::DeferredScene{});
            color = frame_graph.compose(Engine::RenderModules::AntiAliasing{.input = color});

            Engine::FullscreenEffectDescription effect{
                .shader_path = "Shaders/runtime_bloom_threshold.slang",
                .module_name = "runtime_bloom_threshold",
                .fragment_entry_point = "fragmentMain",
                .label = UString{"Runtime bloom threshold view"_ustr},
            };
            effect.set_push_constants(ThresholdConstants{
                .threshold = render_graph_.bloom().threshold,
                .soft_knee = render_graph_.bloom().soft_knee,
            });
            color = frame_graph.compose(Engine::RenderModules::FullscreenEffect{
                .input = color,
                .effect = std::move(effect),
            });
            color = frame_graph.compose(Engine::RenderModules::ToneMapping{.input = color});
            color = frame_graph.compose(Engine::RenderModules::DebugOverlay{.input = color});
            (void)frame_graph.compose(Engine::RenderModules::Present{.input = color});
        }
        if (auto fly = engine.ecs_world().get_component<FlyCameraState>(camera_control_entity_)) {
            constexpr f32 move_speed_meters_per_second = 4.0f;
            constexpr f32 look_degrees_per_pixel = 0.15f;
            constexpr f32 max_pitch_degrees = 89.0f;
            const f32 dt = static_cast<f32>(frame.delta_seconds);

            glm::vec3 move{0.0f};
            if (fly->move_forward) move += camera_.forward();
            if (fly->move_backward) move -= camera_.forward();
            if (fly->move_right) move += camera_.right();
            if (fly->move_left) move -= camera_.right();
            if (fly->move_up) move += glm::vec3{0.0f, 1.0f, 0.0f};
            if (fly->move_down) move -= glm::vec3{0.0f, 1.0f, 0.0f};
            if (glm::dot(move, move) > 0.0f) {
                camera_.translate_world(glm::normalize(move) * move_speed_meters_per_second * dt);
            }

            if (fly->look_delta_x != 0.0f || fly->look_delta_y != 0.0f) {
                fly->yaw_degrees -= fly->look_delta_x * look_degrees_per_pixel;
                fly->pitch_degrees = std::clamp(
                    fly->pitch_degrees - fly->look_delta_y * look_degrees_per_pixel,
                    -max_pitch_degrees, max_pitch_degrees);
                camera_.set_euler_degrees({fly->pitch_degrees, fly->yaw_degrees, 0.0f});
                fly->look_delta_x = 0.0f;
                fly->look_delta_y = 0.0f;
            }
        }
        camera_.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);

        Engine::RenderFrameParameters parameters{
            .camera = camera_,
            .lighting = Engine::SceneLighting{
                .ambient_radiance = {0.035f, 0.04f, 0.055f},
                .exposure = 1.0f,
            },
            .render_graph = std::move(frame_graph),
            .ui_overlay = build_tweak_panel_overlay(engine, surface, frame),
            .debug_label = UString{"Runtime ECS scene"_ustr},
        };


        camera_.commit_frame();
        return parameters;
    }

    /// Handles the on shutdown callback and updates the associated platform state.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void RuntimeDemoGameLogic::on_shutdown(Engine::Engine &           ) noexcept {}

    /// Creates a runtime demo game logic from the supplied parameters.
    ///
    /// @return Returns the current create runtime demo game logic value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::unique_ptr<Engine::GameLogic> create_runtime_demo_game_logic() {
        return std::make_unique<RuntimeDemoGameLogic>();
    }

} // namespace SFT::Runtime
