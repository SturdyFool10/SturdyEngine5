#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <cmath>
#include <expected>
#include <format>

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

#include <Engine/EngineModule.hpp>
#include <Core/Core.hpp>
#include <Renderer/Renderer.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>

using std::array;
using std::format;
using std::span;
using std::string_view;

using std::unexpected;

namespace RendererApi = SFT::Renderer;

namespace SFT::Engine {

    // The API-selection switch point now lives inside SFT::Renderer::Renderer. Engine owns the
    // high-level renderer, not the raw graphics backend.
    Engine::Engine() = default;

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

        if (Core::RendererResult demo_scene = create_demo_scene(); !demo_scene.has_value()) {
            return unexpected(demo_scene.error());
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

    void Engine::on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept {
        renderer_.on_surface_resize_needed(surface);
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

    Core::RendererResult Engine::create_demo_scene() {
        Core::Slang::ShaderCompileOptions shader_options{
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

        auto material_template = renderer_.create_material_template_from_source(
            Core::Slang::ShaderSource::from_file("Shaders/gbuffer_geometry.slang", "demo_gbuffer_geometry"),
            shader_options,
            "deferred demo gbuffer material template");
        if (!material_template) {
            return unexpected(material_template.error());
        }
        demo_scene_.material_template = *material_template;

        auto material_instance = renderer_.create_material_instance(demo_scene_.material_template,
                                                                    "deferred demo gbuffer material");
        if (!material_instance) {
            return unexpected(material_instance.error());
        }
        demo_scene_.material_instance = *material_instance;

        auto upload_colored = [this](RendererApi::Mesh mesh, const glm::vec4 &color) -> Core::RendererExpected<RendererApi::MeshHandle> {
            mesh.set_vertex_color(color);
            return renderer_.upload(mesh);
        };

        auto floor = upload_colored(RendererApi::Mesh::plane(RendererApi::PlaneParams{
                                        .width = 8.0f,
                                        .depth = 8.0f,
                                        .width_segments = 16,
                                        .depth_segments = 16,
                                    }, "demo floor"),
                                    glm::vec4{0.55f, 0.58f, 0.62f, 1.0f});
        if (!floor) return unexpected(floor.error());
        demo_scene_.floor = *floor;

        auto sphere = upload_colored(RendererApi::Mesh::uv_sphere(RendererApi::UvSphereParams{
                                         .radius = 0.75f,
                                         .rings = 32,
                                         .segments = 64,
                                     }, "demo sphere"),
                                     glm::vec4{0.92f, 0.28f, 0.20f, 1.0f});
        if (!sphere) return unexpected(sphere.error());
        demo_scene_.sphere = *sphere;

        auto cube = upload_colored(RendererApi::Mesh::cube(RendererApi::CubeParams{.size = 1.15f}, "demo cube"),
                                   glm::vec4{0.18f, 0.46f, 0.95f, 1.0f});
        if (!cube) return unexpected(cube.error());
        demo_scene_.cube = *cube;

        auto torus = upload_colored(RendererApi::Mesh::torus(RendererApi::TorusParams{
                                        .major_radius = 0.55f,
                                        .minor_radius = 0.18f,
                                        .major_segments = 64,
                                        .minor_segments = 24,
                                    }, "demo torus"),
                                    glm::vec4{0.95f, 0.72f, 0.18f, 1.0f});
        if (!torus) return unexpected(torus.error());
        demo_scene_.torus = *torus;

        auto cone = upload_colored(RendererApi::Mesh::cone(RendererApi::ConeParams{
                                      .radius = 0.65f,
                                      .height = 1.25f,
                                      .radial_segments = 48,
                                      .axis = RendererApi::Axis::Y,
                                      .capped = true,
                                  }, "demo cone"),
                                  glm::vec4{0.35f, 0.88f, 0.52f, 1.0f});
        if (!cone) return unexpected(cone.error());
        demo_scene_.cone = *cone;

        demo_scene_.ready = true;
        return {};
    }

    Core::RendererResult Engine::render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) {
        if (!demo_scene_.ready) {
            return renderer_.render_frame(surface, frame);
        }

        const glm::vec3 camera_position{4.0f, 2.35f, 5.75f};
        RendererApi::CameraView camera{};
        camera.view = glm::lookAtRH(camera_position, glm::vec3{0.0f, 0.35f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
        const f32 aspect = frame.framebuffer_height != 0
                               ? static_cast<f32>(frame.framebuffer_width) / static_cast<f32>(frame.framebuffer_height)
                               : 16.0f / 9.0f;
        camera.projection = glm::perspectiveRH_ZO(glm::radians(55.0f), aspect, 0.05f, 200.0f);
        camera.projection[1][1] *= -1.0f;
        camera.world_position = camera_position;
        camera.near_plane = 0.05f;
        camera.far_plane = 200.0f;
        camera.vertical_fov_radians = glm::radians(60.0f);

        const auto translate = [](glm::vec3 position) {
            return glm::translate(glm::mat4{1.0f}, position);
        };

        const array<RendererApi::SceneRenderable, 5> renderables{
            RendererApi::SceneRenderable{
                .mesh = demo_scene_.floor,
                .material = demo_scene_.material_instance,
                .world_transform = translate({0.0f, -0.55f, 0.0f}),
                .stable_id = 1,
            },
            RendererApi::SceneRenderable{
                .mesh = demo_scene_.sphere,
                .material = demo_scene_.material_instance,
                .world_transform = translate({-1.35f, 0.2f, 0.0f}),
                .stable_id = 2,
            },
            RendererApi::SceneRenderable{
                .mesh = demo_scene_.cube,
                .material = demo_scene_.material_instance,
                .world_transform = translate({1.25f, 0.05f, -0.35f}) * glm::rotate(glm::mat4{1.0f}, glm::radians(18.0f), glm::vec3{0.0f, 1.0f, 0.0f}),
                .stable_id = 3,
            },
            RendererApi::SceneRenderable{
                .mesh = demo_scene_.torus,
                .material = demo_scene_.material_instance,
                .world_transform = translate({0.0f, 0.45f, 1.45f}) * glm::rotate(glm::mat4{1.0f}, glm::radians(72.0f), glm::vec3{1.0f, 0.0f, 0.0f}),
                .stable_id = 4,
            },
            RendererApi::SceneRenderable{
                .mesh = demo_scene_.cone,
                .material = demo_scene_.material_instance,
                .world_transform = translate({0.2f, 0.15f, -1.65f}),
                .stable_id = 5,
            },
        };

        RendererApi::RenderFrameDesc desc{};
        desc.surface = surface;
        desc.frame = frame;
        desc.view.camera = camera;
        desc.view.lighting = RendererApi::SceneLighting{.ambient_radiance = {0.035f, 0.04f, 0.055f}, .exposure = 1.0f};
        desc.view.renderables = span<const RendererApi::SceneRenderable>{renderables.data(), renderables.size()};
        desc.view.debug_label = "deferred demo scene";
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
