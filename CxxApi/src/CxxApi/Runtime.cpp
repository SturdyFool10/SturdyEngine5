#include <CxxApi/Runtime.hpp>

#include <atomic>
#include <string_view>
#include <utility>

#include <RHI/Features.hpp>

#include <CxxApi/Camera.hpp>
#include <CxxApi/Conversions.hpp>
#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {

        namespace E = SFT::Engine;
        namespace C = SFT::Core;
        namespace W = SFT::WindowManager;

        static_assert(static_cast<int>(C::VSyncMode::Adaptive) == static_cast<int>(VSync::Adaptive));
        static_assert(static_cast<int>(C::VariableRefreshMode::Preferred) == static_cast<int>(VariableRefresh::Preferred));
        static_assert(static_cast<int>(C::LatencyMode::Ultra) == static_cast<int>(LatencyMode::Ultra));
        static_assert(static_cast<int>(C::PresentationPreference::PowerEfficient) ==
                      static_cast<int>(PresentationPreference::PowerEfficient));
        static_assert(static_cast<int>(C::HdrColorSpaceMode::DolbyVision) == static_cast<int>(HdrColorSpace::DolbyVision));
        static_assert(static_cast<int>(C::FrameRateLimitMode::MatchDisplayRefresh) ==
                      static_cast<int>(FrameRateLimit::MatchDisplayRefresh));
        static_assert(static_cast<int>(W::WindowMode::ExclusiveFullscreen) == static_cast<int>(WindowMode::ExclusiveFullscreen));
        static_assert(static_cast<int>(RHI::BackendType::WebGpu) == static_cast<int>(GraphicsBackend::WebGpu));

        /// The engine's factory is a bare function pointer, so the callbacks are handed over through
        /// this slot: `runtime_run` fills it immediately before `Runtime::run` and the factory drains it.
        std::unique_ptr<GameLogicCallbacks> g_pending_logic;
        std::atomic<bool> g_runtime_running{false};

        class CallbackGameLogic final : public E::GameLogic {
          public:
            explicit CallbackGameLogic(std::unique_ptr<GameLogicCallbacks> callbacks) : callbacks_(std::move(callbacks)) {}

            E::GameLogicResult on_engine_initialized(E::Engine &engine) override {
                engine_configure_default_render_extraction(engine);
                std::string error;
                if (callbacks_->on_engine_initialized(engine, error)) {
                    return {};
                }
                return std::unexpected(E::GameLogicError{to_ustring(error.empty() ? std::string_view{"game logic failed to initialize"} : error)});
            }

            std::optional<E::RenderFrameParameters> request_render_frame(E::Engine &engine, C::RenderSurfaceHandle surface,
                                                                         const C::FrameInput &input) override {
                E::RenderFrameParameters parameters{};
                // Default to an engine-managed camera so a frame that never sets one still has
                // coherent temporal history.
                parameters.engine_managed_camera_history = true;
                const FrameInfo info{
                    .delta_seconds = input.delta_seconds,
                    .frame_index = input.frame_index,
                    .framebuffer_width = input.framebuffer_width,
                    .framebuffer_height = input.framebuffer_height,
                    .live_resize = input.live_resize,
                    .window_id = static_cast<std::uint64_t>(surface.window_id),
                };
                FrameBuilder frame{engine, surface, info, parameters};
                if (!callbacks_->request_render_frame(engine, info, frame)) {
                    return std::nullopt;
                }
                return parameters;
            }

            void on_shutdown(E::Engine &engine) noexcept override { callbacks_->on_shutdown(engine); }

          private:
            std::unique_ptr<GameLogicCallbacks> callbacks_;
        };

        std::unique_ptr<E::GameLogic> make_logic() { return std::make_unique<CallbackGameLogic>(std::move(g_pending_logic)); }

        [[nodiscard]] RHI::Feature feature_by_name(const std::string &name) {
            for (std::size_t i = 0; i < RHI::feature_count; ++i) {
                const auto feature = static_cast<RHI::Feature>(i);
                const char *candidate = RHI::feature_name(feature);
                if (candidate != nullptr && std::string_view{candidate} == name) {
                    return feature;
                }
            }
            fail("unknown RHI feature name '" + name + "'");
        }

    } // namespace

    // ---- runtime configuration ----

    RuntimeOptions runtime_options_defaults() noexcept {
        const RuntimeConfig defaults{};
        return runtime_config_options(defaults);
    }

    std::unique_ptr<RuntimeConfig> runtime_config_new() {
        auto config = std::make_unique<RuntimeConfig>();
        config->config.application.primary_window.graphics_api = W::WindowGraphicsApi::Vulkan;
        return config;
    }

    void runtime_config_set_options(RuntimeConfig &config, const RuntimeOptions &o) noexcept {
        auto &app = config.config.application;
        auto &window = app.primary_window;
        window.extent = {o.width, o.height};
        window.resizable = o.resizable;
        window.decorated = o.decorated;
        window.high_dpi = o.high_dpi;
        window.mode = static_cast<W::WindowMode>(o.window_mode);
        window.transparent = o.window_transparent;
        window.visible = o.window_visible;
        window.use_default_position = !o.has_window_position;
        window.position = {o.window_x, o.window_y};
        app.enable_runtime_window_management = o.runtime_window_management;
        if (o.title_update_interval_seconds > 0.0) {
            app.primary_window_title_update_interval_seconds = o.title_update_interval_seconds;
        } else {
            app.primary_window_title_update_interval_seconds.reset();
        }
        auto &engine = app.engine;
        engine.graphics_backend = static_cast<RHI::BackendType>(o.graphics_backend);
        engine.enable_shader_disk_cache = o.enable_shader_disk_cache;
        auto &features = engine.features;
        features.raytracing = o.raytracing;
        features.prefer_async_compute = o.prefer_async_compute;
        features.desired_frames_in_flight = o.desired_frames_in_flight != 0 ? o.desired_frames_in_flight : C::RendererFeatureRequest{}.desired_frames_in_flight;
        features.enable_native_access_extension = o.enable_native_access;
        auto &p = features.presentation;
        const PresentationOptions &src = o.presentation;
        p.vsync = static_cast<C::VSyncMode>(src.vsync);
        p.variable_refresh = static_cast<C::VariableRefreshMode>(src.variable_refresh);
        p.latency = static_cast<C::LatencyMode>(src.latency);
        p.preference = static_cast<C::PresentationPreference>(src.preference);
        p.hdr_enabled = src.hdr_enabled;
        p.hdr_color_space = static_cast<C::HdrColorSpaceMode>(src.hdr_color_space);
        p.transparent_composition = src.transparent_composition;
        p.swapchain_image_count = src.swapchain_image_count;
        p.allow_present_from_compute = src.allow_present_from_compute;
        p.frame_rate_limit_mode = static_cast<C::FrameRateLimitMode>(src.frame_rate_limit);
        p.frame_rate_limit_fps = src.frame_rate_limit_fps;
        p.variable_refresh_margin_fps = src.variable_refresh_margin_fps;
        p.unfocused_frame_rate_limit_fps = src.unfocused_frame_rate_limit_fps;
        p.snap_frame_rate_limit_to_refresh = src.snap_frame_rate_limit_to_refresh;
    }

    RuntimeOptions runtime_config_options(const RuntimeConfig &config) noexcept {
        const auto &app = config.config.application;
        const auto &window = app.primary_window;
        const auto &features = app.engine.features;
        const auto &p = features.presentation;
        RuntimeOptions o{};
        o.width = window.extent.x;
        o.height = window.extent.y;
        o.resizable = window.resizable;
        o.decorated = window.decorated;
        o.high_dpi = window.high_dpi;
        o.window_mode = static_cast<WindowMode>(window.mode);
        o.window_transparent = window.transparent;
        o.window_visible = window.visible;
        o.has_window_position = !window.use_default_position;
        o.window_x = window.position.x;
        o.window_y = window.position.y;
        o.title_update_interval_seconds = app.primary_window_title_update_interval_seconds.value_or(0.0);
        o.runtime_window_management = app.enable_runtime_window_management;
        o.graphics_backend = static_cast<GraphicsBackend>(app.engine.graphics_backend);
        o.raytracing = static_cast<bool>(features.raytracing);
        o.prefer_async_compute = static_cast<bool>(features.prefer_async_compute);
        o.desired_frames_in_flight = features.desired_frames_in_flight;
        o.enable_native_access = static_cast<bool>(features.enable_native_access_extension);
        o.enable_shader_disk_cache = app.engine.enable_shader_disk_cache;
        o.presentation = PresentationOptions{
            .vsync = static_cast<VSync>(p.vsync),
            .variable_refresh = static_cast<VariableRefresh>(p.variable_refresh),
            .latency = static_cast<LatencyMode>(p.latency),
            .preference = static_cast<PresentationPreference>(p.preference),
            .hdr_enabled = static_cast<bool>(p.hdr_enabled),
            .hdr_color_space = static_cast<HdrColorSpace>(p.hdr_color_space),
            .transparent_composition = static_cast<bool>(p.transparent_composition),
            .swapchain_image_count = p.swapchain_image_count,
            .allow_present_from_compute = static_cast<bool>(p.allow_present_from_compute),
            .frame_rate_limit = static_cast<FrameRateLimit>(p.frame_rate_limit_mode),
            .frame_rate_limit_fps = p.frame_rate_limit_fps,
            .variable_refresh_margin_fps = p.variable_refresh_margin_fps,
            .unfocused_frame_rate_limit_fps = p.unfocused_frame_rate_limit_fps,
            .snap_frame_rate_limit_to_refresh = static_cast<bool>(p.snap_frame_rate_limit_to_refresh),
        };
        return o;
    }

    void runtime_config_set_window_title(RuntimeConfig &config, const std::string &title) { config.window_title = title; }
    void runtime_config_set_app_name(RuntimeConfig &config, const std::string &name) { config.app_name = name; }
    void runtime_config_set_shaders_directory(RuntimeConfig &config, const std::string &directory) {
        config.config.application.engine.shaders_directory = directory;
    }
    void runtime_config_set_graphics_device_id(RuntimeConfig &config, const std::string &device_id) {
        config.config.application.engine.graphics_physical_device_id = device_id;
    }
    void runtime_config_require_feature(RuntimeConfig &config, const std::string &feature) {
        config.config.application.engine.features.required_rhi_features.set(feature_by_name(feature));
    }
    void runtime_config_request_feature(RuntimeConfig &config, const std::string &feature) {
        config.config.application.engine.features.optional_rhi_features.set(feature_by_name(feature));
    }

    std::unique_ptr<std::vector<std::string>> rhi_feature_names() {
        auto names = std::make_unique<std::vector<std::string>>();
        names->reserve(RHI::feature_count);
        for (std::size_t i = 0; i < RHI::feature_count; ++i) {
            if (const char *name = RHI::feature_name(static_cast<RHI::Feature>(i))) {
                names->emplace_back(name);
            }
        }
        return names;
    }

    std::int32_t runtime_run(std::unique_ptr<RuntimeConfig> config, const std::vector<std::string> &args,
                             std::unique_ptr<GameLogicCallbacks> logic) {
        if (!config || !logic) {
            fail("runtime_run needs a configuration and game logic");
        }
        bool expected = false;
        if (!g_runtime_running.compare_exchange_strong(expected, true)) {
            fail("a runtime is already running in this process");
        }
        struct RunningGuard {
            ~RunningGuard() {
                g_pending_logic.reset();
                g_runtime_running.store(false);
            }
        } guard;

        // Strings the engine config refers to by pointer live in `config` for the whole run.
        config->config.application.primary_window.title = config->window_title.c_str();
        config->config.application.engine.app_name = config->app_name.c_str();
        config->config.primary_window_title = to_ustring(config->window_title);

        const Foundation::CliArgs cli(args.begin(), args.end());
        g_pending_logic = std::move(logic);
        return static_cast<std::int32_t>(Runtime::run(cli, config->config, &make_logic));
    }

    void engine_configure_default_render_extraction(E::Engine &engine) {
        namespace Ecs = SFT::Ecs;
        engine.ecs_world().bind_resource(engine.render_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity, const E::WorldTransform &transform, const E::ModelRenderer &model_renderer,
               Ecs::WriteResource<E::RenderFrameRequests> render) noexcept { render->submit(entity, transform, model_renderer); });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity, const E::WorldTransform &transform, const E::LightGizmoRenderer &gizmo,
               Ecs::WriteResource<E::RenderFrameRequests> render) noexcept { render->submit_gizmo(entity, transform, gizmo); });
        engine.ecs_world().bind_resource(engine.light_frame_requests());
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity, const E::WorldTransform &transform, const E::DirectionalLightRenderer &light,
               Ecs::WriteResource<E::LightFrameRequests> lights) noexcept { lights->submit(entity, transform, light); });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity, const E::WorldTransform &transform, const E::SpotLightRenderer &light,
               Ecs::WriteResource<E::LightFrameRequests> lights) noexcept { lights->submit(entity, transform, light); });
        engine.render_extraction_schedule().add_system(
            [](Ecs::Entity entity, const E::WorldTransform &transform, const E::PointLightRenderer &light,
               Ecs::WriteResource<E::LightFrameRequests> lights) noexcept { lights->submit(entity, transform, light); });
    }

    // ---- per-frame description ----

    FrameInfo frame_info(const FrameBuilder &frame) noexcept { return frame.info; }

    void frame_set_camera(FrameBuilder &frame, const CameraDesc &desc) {
        E::Camera camera{};
        camera_apply_desc(camera, desc);
        if (desc.reset_history) {
            frame.engine.reset_camera_history(frame.surface);
        }
        frame.parameters.camera = camera;
        frame.parameters.engine_managed_camera_history = true;
    }

    void frame_set_camera_object(FrameBuilder &frame, const E::Camera &camera) {
        frame.parameters.camera = camera;
        frame.parameters.engine_managed_camera_history = false;
    }

    void frame_set_lighting(FrameBuilder &frame, const SceneLightingDesc &lighting) noexcept {
        frame.parameters.lighting.ambient_radiance = to_vec3(lighting.ambient_radiance);
        frame.parameters.lighting.exposure = lighting.exposure;
    }

    void frame_set_debug_label(FrameBuilder &frame, const std::string &label) { frame.parameters.debug_label = to_ustring(label); }

} // namespace SFT::CxxApi
