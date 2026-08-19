#include <RuntimeDemoGameLogic/RuntimeDemoGameLogic.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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
        }
        render_graph_.bloom().enabled = !threshold_view;

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
