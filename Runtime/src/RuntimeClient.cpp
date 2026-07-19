#include <Runtime/src/RuntimeClient.hpp>

#include <format>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace RendererApi = SFT::Renderer;

namespace SFT::Runtime {

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
        spawn_demo_entities(engine);
        return {};
    }

    UString RuntimeClient::primary_window_title(
        Engine::Engine &engine,
        const Engine::ApplicationFrameStats &stats) {
        (void)engine;
        return UString{std::format(
            "SturdyEngine 5 Frame Time: {}, FPS: {:.0f} [{} window(s)]",
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
        (void)engine;
        (void)surface;

        camera_.set_viewport_size(frame.framebuffer_width, frame.framebuffer_height);

        return Engine::RenderFrameParameters{
            .camera = camera_,
            .lighting = Engine::SceneLighting{
                .ambient_radiance = {0.035f, 0.04f, 0.055f},
                .exposure = 1.0f,
            },
            .render_graph = render_graph_,
            .debug_label = UString{"Runtime ECS deferred scene"_ustr},
        };
    }

} // namespace SFT::Runtime
