#include <Runtime/src/RuntimeClient.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <glm/ext/matrix_transform.hpp>
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
        camera_.set_position({4.0f, 2.35f, 5.75f});
        camera_.look_at({0.0f, 0.35f, 0.0f});

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
        return UString{std::format(
            "SturdyEngine 5 | Bloom threshold: {:.2f}{} | Frame: {}, FPS: {:.0f} [{} window(s)]",
            threshold,
            threshold_view ? " [threshold view]" : "",
            Foundation::human_readable_time(stats.frame_seconds),
            stats.frame_seconds > 0.0 ? 1.0 / stats.frame_seconds : 0.0,
            stats.window_count)};
    }



    Engine::AssetResult RuntimeClient::create_demo_content(Engine::Engine &engine) {
        Engine::AssetManager &assets = engine.assets();
        auto shader = assets.load_shader(
            "Shaders/gbuffer_geometry.slang",
            UString{"runtime demo gbuffer shader"_ustr});
        if (!shader) {
            return std::unexpected(shader.error());
        }
        demo_.shader = *shader;

        auto floor = assets.create_model(
            RendererApi::Mesh::plane(
                RendererApi::PlaneParams{
                    .width = 8.0f,
                    .depth = 8.0f,
                    .width_segments = 16,
                    .depth_segments = 16,
                },
                "runtime demo floor"),
            demo_.shader,
            glm::vec4{0.55f, 0.58f, 0.62f, 1.0f});
        if (!floor) {
            return std::unexpected(floor.error());
        }
        demo_.floor = *floor;

        auto sphere = assets.create_model(
            RendererApi::Mesh::uv_sphere(
                RendererApi::UvSphereParams{
                    .radius = 0.75f,
                    .rings = 32,
                    .segments = 64,
                },
                "runtime demo sphere"),
            demo_.shader,
            glm::vec4{0.92f, 0.28f, 0.20f, 1.0f});
        if (!sphere) {
            return std::unexpected(sphere.error());
        }
        demo_.sphere = *sphere;

        auto cube = assets.create_model(
            RendererApi::Mesh::cube(RendererApi::CubeParams{.size = 1.15f}, "runtime demo cube"),
            demo_.shader,
            glm::vec4{0.18f, 0.46f, 0.95f, 1.0f});
        if (!cube) {
            return std::unexpected(cube.error());
        }
        demo_.cube = *cube;

        auto torus = assets.create_model(
            RendererApi::Mesh::torus(
                RendererApi::TorusParams{
                    .major_radius = 0.55f,
                    .minor_radius = 0.18f,
                    .major_segments = 64,
                    .minor_segments = 24,
                },
                "runtime demo torus"),
            demo_.shader,
            glm::vec4{0.95f, 0.72f, 0.18f, 1.0f});
        if (!torus) {
            return std::unexpected(torus.error());
        }
        demo_.torus = *torus;

        auto cone = assets.create_model(
            RendererApi::Mesh::cone(
                RendererApi::ConeParams{
                    .radius = 0.65f,
                    .height = 1.25f,
                    .radial_segments = 48,
                    .axis = RendererApi::Axis::Y,
                    .capped = true,
                },
                "runtime demo cone"),
            demo_.shader,
            glm::vec4{0.35f, 0.88f, 0.52f, 1.0f});
        if (!cone) {
            return std::unexpected(cone.error());
        }
        demo_.cone = *cone;
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
    }

    void RuntimeClient::spawn_demo_entities(Engine::Engine &engine) {
        const auto translate = [](glm::vec3 position) {
            return glm::translate(glm::mat4{1.0f}, position);
        };
        const auto spawn_renderable = [&engine](glm::mat4 transform, Engine::Asset model) {
            (void)engine.ecs_world().spawn(
                Engine::WorldTransform{.value = transform},
                Engine::ModelRenderer{.model = model});
        };

        spawn_renderable(translate({0.0f, -0.55f, 0.0f}), demo_.floor);
        spawn_renderable(translate({-1.35f, 0.2f, 0.0f}), demo_.sphere);
        spawn_renderable(
            translate({1.25f, 0.05f, -0.35f}) *
                glm::rotate(glm::mat4{1.0f}, glm::radians(18.0f), glm::vec3{0.0f, 1.0f, 0.0f}),
            demo_.cube);
        spawn_renderable(
            translate({0.0f, 0.45f, 1.45f}) *
                glm::rotate(glm::mat4{1.0f}, glm::radians(72.0f), glm::vec3{1.0f, 0.0f, 0.0f}),
            demo_.torus);
        spawn_renderable(translate({0.2f, 0.15f, -1.65f}), demo_.cone);
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
        camera_.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);

        return Engine::RenderFrameParameters{
            .camera = camera_,
            .lighting = Engine::SceneLighting{
                .ambient_radiance = {0.035f, 0.04f, 0.055f},
                .exposure = 1.0f,
            },
            .render_graph = render_graph_,
            .custom_post_processes = std::move(custom_post_processes),
            .debug_label = UString{"Runtime ECS deferred scene"_ustr},
        };
    }

} // namespace SFT::Runtime
