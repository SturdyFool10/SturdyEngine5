#include <Runtime/src/RuntimeClient.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <span>
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

    RuntimeClient::RuntimeClient() {
        config_.primary_window.title = "Sturdy Engine 5";
        config_.primary_window.extent = {1280, 720};
        config_.primary_window.graphics_api = Platform::Windowing::WindowGraphicsApi::Vulkan;
        config_.engine.app_name = "Sturdy Engine 5";
        config_.engine.shaders_directory = "Shaders";
        config_.primary_window_title_update_interval_seconds = 0.25;

        camera_ = Engine::Camera::perspective(55.0f, 16.0f / 9.0f, 0.05f, 200.0f);
        // Sponza (see create_demo_content()) is roughly 25m long — stand at one end of the colonnade
        // hall looking down its length. Fly around with WASD+QE / right-drag mouse-look either way.
        camera_.set_position({0.0f, 3.0f, 9.0f});
        camera_.look_at({0.0f, 3.0f, 0.0f});

        // High-level frame recipe: Runtime selects semantic features and image policy while Engine
        // keeps graph resources, barriers, pass callbacks and synchronization private.
        render_graph_ = Engine::RenderGraph::standard();
        render_graph_
            .set_resolution_scale(1.0f)
            .set_tone_mapping(Engine::ToneMappingOperator::PsychoV, 1.0f)
            .configure_bloom([](Engine::BloomSettings &bloom) { bloom.threshold = 3.20f; })
            .enable(Engine::RenderFeature::DebugOverlay);
    }

    const Engine::ApplicationConfig &RuntimeClient::application_config() const noexcept {
        return config_;
    }

    Engine::ApplicationResult RuntimeClient::on_engine_initialized(Engine::Engine &engine) {
        if (Engine::AssetResult content = create_demo_content(engine); !content) {
            return std::unexpected(Engine::ApplicationError{.message = content.error().message});
        }
        configure_render_extraction(engine);
        configure_event_systems(engine);
        spawn_demo_entities(engine);
        return {};
    }

    UString RuntimeClient::primary_window_title(
        Engine::Engine &engine,
        const Engine::ApplicationFrameStats &stats) {
        f32 threshold = render_graph_.bloom().threshold;
        bool threshold_view = false;
        if (auto state = engine.ecs_world().get_component<BloomTuningState>(bloom_controls_entity_)) {
            threshold = state->threshold;
            threshold_view = state->threshold_view;
        }
        return UString{
            "SturdyEngine 5"};
    }



    Engine::AssetResult RuntimeClient::create_demo_content(Engine::Engine &engine) {
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

        auto gizmo_shader = assets.load_shader(Engine::ShaderAssetDesc{
            .source = "Shaders/geometry_color.slang",
            .label = UString{"light gizmo shader"_ustr},
        });
        if (!gizmo_shader) {
            return std::unexpected(gizmo_shader.error());
        }
        gizmo_shader_ = *gizmo_shader;

#ifdef STURDY_GLTF_SAMPLE_ASSETS_DIR
        // Sponza is the only scene Runtime shows right now — kept deliberately simple, one thing at
        // a time. Only present when configured with -DSTURDY_FETCH_SAMPLE_ASSETS=ON, which fetches
        // the (large, non-build) sample-assets repo; without it Runtime builds but shows nothing
        // (CI never runs Runtime, only builds it, so this is a safe default for other build configs).
        // Sponza is a plain-JSON .gltf with external .bin/.jpg/.png files (unlike a .glb's embedded
        // buffer views), so this also exercises import_gltf()'s external-URI image-loading path.
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

    void RuntimeClient::configure_render_extraction(Engine::Engine &engine) {
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

    void RuntimeClient::configure_event_systems(Engine::Engine &engine) {
        bloom_threshold_events_.build(engine.ecs_world(), engine.update_schedule());
        bloom_controls_entity_ = engine.ecs_world().spawn(
            BloomKeyboardControls{.threshold_step = 0.05f},
            BloomTuningState{.threshold = render_graph_.bloom().threshold});

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

        // A second system consumes Runtime's own event type, demonstrating that API consumers can
        // create event pipelines on top of Engine's built-in input streams without engine changes.
        engine.update_schedule().add_system(
            [](Ecs::EventReader<BloomThresholdChanged> changes) noexcept {
                for (const BloomThresholdChanged &change : changes.read()) {
                    Foundation::log_info("Bloom threshold: {:.2f}; threshold view: {}",
                                         change.threshold, change.threshold_view ? "on" : "off");
                }
            });

        // Free-fly camera: WASD+QE held state and right-drag mouse-look deltas are tracked here;
        // request_render_frame() applies the actual per-frame movement (it has real delta time,
        // Core::FrameInput::delta_seconds — see FlyCameraState's own comment).
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
                // SDL's motion-event button-state mask; bit 2 (0x4) is the right mouse button
                // (SDL_BUTTON_MASK(SDL_BUTTON_RIGHT) == 1u << (3 - 1)) — matches
                // Platform/src/Platform/Window/SDL3/SDL3Impl.cpp's raw event.motion.state passthrough.
                constexpr u32 right_mouse_button_mask = 0x4u;
                for (const Engine::MouseMoveEvent &event : mouse.read()) {
                    if ((event.mouse.buttons & right_mouse_button_mask) != 0) {
                        state.look_delta_x += event.mouse.delta_x;
                        state.look_delta_y += event.mouse.delta_y;
                    }
                }
            });
    }

    void RuntimeClient::spawn_demo_entities(Engine::Engine &engine) {
#ifdef STURDY_GLTF_SAMPLE_ASSETS_DIR
        // Every resolved Sponza node instance is spawned as-is, at its own baked world transform.
        // glTF's coordinate convention (Y-up, right-handed) already matches this engine's, so no
        // basis-change rotation is needed.
        for (const Engine::GltfNodeInstance &instance : gltf_instances_) {
            if (!instance.model) {
                continue;
            }
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = instance.world_transform},
                Engine::ModelRenderer{.model = instance.model});
        }

        // Sponza itself carries no KHR_lights_punctual nodes, but any glTF asset that does gets its
        // lights spawned here alongside its meshes.
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

        // Hand-authored demo lights: one sun plus a few local lights placed along Sponza's
        // colonnade hall so point/spot shading is visible without a light-carrying glTF asset.
        {
            // An identity WorldTransform would leave the sun pointing straight down (its
            // world_direction() reads local -Y as forward) — build a transform that instead faces
            // the same angled direction DirectionalLight's own hand-authored default uses, via the
            // same lookAtRH-inverse-plus-remap technique spawn_spot_light uses below.
            const glm::vec3 sun_direction = glm::normalize(glm::vec3{0.35f, -0.75f, 0.55f});
            glm::mat4 sun_transform =
                glm::inverse(glm::lookAtRH(glm::vec3{0.0f}, sun_direction, glm::vec3{0.0f, 1.0f, 0.0f}));
            // +90 degrees, not -90: inverse(lookAt) sends local -Z to the target direction, and we
            // need local -Y (world_direction()'s forward axis) to land there too — i.e. the rotation
            // that sends -Y to -Z, which is the +90-about-X map (rotate(-90,X) sends -Z to -Y, the
            // opposite composition; using it here silently negates the whole direction instead).
            sun_transform = sun_transform * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = sun_transform},
                Engine::DirectionalLightRenderer{.radiance = {4.0f, 3.75f, 3.35f}});
        }

        // Always-on debug marker at a light's position: a low-poly (base, unsubdivided) icosphere,
        // tinted by the light's own radiance (normalized so a bright light doesn't just look white)
        // — see RuntimeClient.hpp/EcsRendering.hpp's LightGizmoRenderer doc comment for why this
        // needs its own component/pass rather than reusing ModelRenderer.
        const auto spawn_light_gizmo = [&](glm::vec3 position, glm::vec3 radiance) {
            const f32 peak = std::max({radiance.r, radiance.g, radiance.b, 0.001f});
            auto model = engine.assets().create_model(Engine::ModelAssetDesc{
                .label = UString{"light gizmo"_ustr},
                .primitives = {
                    Engine::ModelPrimitiveDesc{
                        .mesh = RendererApi::Mesh::ico_sphere({.radius = 0.15f, .subdivisions = 0}),
                        .shader = gizmo_shader_,
                        .vertex_color = glm::vec4{radiance / peak, 1.0f},
                    },
                },
            });
            if (!model) {
                Foundation::log_error("Failed to create light gizmo model: {}", model.error().message.cpp_string());
                return;
            }
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = glm::translate(glm::mat4{1.0f}, position)},
                Engine::LightGizmoRenderer{.model = *model});
        };

        const auto spawn_point_light = [&](glm::vec3 position, glm::vec3 radiance, f32 range) {
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = glm::translate(glm::mat4{1.0f}, position)},
                Engine::PointLightRenderer{.radiance = radiance, .range = range});
            spawn_light_gizmo(position, radiance);
        };
        spawn_point_light({-6.0f, 2.0f, 0.0f}, {6.0f, 3.5f, 2.0f}, 8.0f);
        spawn_point_light({6.0f, 2.0f, 0.0f}, {2.0f, 3.0f, 6.0f}, 8.0f);

        const auto spawn_spot_light = [&](glm::vec3 position, glm::vec3 target, glm::vec3 radiance) {
            glm::mat4 transform = glm::inverse(glm::lookAtRH(position, target, glm::vec3{0.0f, 1.0f, 0.0f}));
            // +90 degrees — see the sun's identical construction above for the derivation of why
            // -90 (this line's old value) silently negated the whole direction instead of just
            // remapping the axis.
            transform = transform * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = transform},
                Engine::SpotLightRenderer{
                    .radiance = radiance,
                    .range = 15.0f,
                    .inner_cone_cos = 0.95f,
                    .outer_cone_cos = 0.85f,
                });
            spawn_light_gizmo(position, radiance);
        };
        spawn_spot_light({0.75f, 6.26f, 1.34f}, {0.0f, 0.0f, 0.0f}, {5.0f, 4.5f, 4.0f});
#else
        (void)engine;
#endif
    }

    std::optional<Engine::RenderFrameParameters> RuntimeClient::request_render_frame(
        Engine::Engine &engine,
        Core::RenderSurfaceHandle surface,
        const Core::FrameInput &frame) {
        (void)surface;

        bool threshold_view = false;
        if (auto bloom = engine.ecs_world().get_component<BloomTuningState>(bloom_controls_entity_)) {
            render_graph_.bloom().threshold = bloom->threshold;
            threshold_view = bloom->threshold_view;
        }
        render_graph_.bloom().enabled = !threshold_view;

        std::vector<RendererApi::CustomPostProcessEffect> custom_post_processes;
        if (threshold_view) {
            const ThresholdConstants constants{
                .threshold = render_graph_.bloom().threshold,
                .soft_knee = render_graph_.bloom().soft_knee,
            };
            const std::span<const std::byte> bytes = std::as_bytes(std::span{&constants, 1});
            RendererApi::CustomPostProcessEffect effect{
                .shader_path = "Shaders/runtime_bloom_threshold.slang",
                .module_name = "runtime_bloom_threshold",
                .fragment_entry_point = "fragmentMain",
                .label = UString{"Runtime bloom threshold view"_ustr},
            };
            effect.push_constants.assign(bytes.begin(), bytes.end());
            custom_post_processes.push_back(std::move(effect));
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

        return Engine::RenderFrameParameters{
            .camera = camera_,
            .lighting = Engine::SceneLighting{
                .ambient_radiance = {0.035f, 0.04f, 0.055f},
                .exposure = 1.0f,
            },
            .render_graph = render_graph_,
            .custom_post_processes = std::move(custom_post_processes),
            .debug_label = UString{"Runtime ECS scene"_ustr},
        };
    }

} // namespace SFT::Runtime
