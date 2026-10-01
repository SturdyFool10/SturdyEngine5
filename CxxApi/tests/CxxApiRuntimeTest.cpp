#include <CxxApi/Camera.hpp>
#include <CxxApi/Error.hpp>
#include <CxxApi/Runtime.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(condition)                                                                                        \
    ((condition) ? static_cast<void>(0)                                                                         \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using namespace SFT::CxxApi;

    void options_round_trip() {
        auto config = runtime_config_new();
        RuntimeOptions options = runtime_config_options(*config);
        const RuntimeOptions defaults = runtime_options_defaults();
        CHECK(options.width == defaults.width && options.height == defaults.height);

        options.width = 1600;
        options.height = 900;
        options.window_mode = WindowMode::BorderlessFullscreen;
        options.has_window_position = true;
        options.window_x = 40;
        options.window_y = -20;
        options.title_update_interval_seconds = 0.5;
        options.presentation.vsync = VSync::Adaptive;
        options.presentation.hdr_color_space = HdrColorSpace::DolbyVision;
        options.presentation.frame_rate_limit = FrameRateLimit::Custom;
        options.presentation.frame_rate_limit_fps = 144.0;
        options.desired_frames_in_flight = 3;
        runtime_config_set_options(*config, options);

        const RuntimeOptions back = runtime_config_options(*config);
        CHECK(back.width == 1600 && back.height == 900);
        CHECK(back.window_mode == WindowMode::BorderlessFullscreen);
        CHECK(back.has_window_position && back.window_x == 40 && back.window_y == -20);
        CHECK(back.title_update_interval_seconds == 0.5);
        CHECK(back.presentation.vsync == VSync::Adaptive);
        CHECK(back.presentation.hdr_color_space == HdrColorSpace::DolbyVision);
        CHECK(back.presentation.frame_rate_limit == FrameRateLimit::Custom);
        CHECK(back.presentation.frame_rate_limit_fps == 144.0);
        CHECK(back.desired_frames_in_flight == 3);
    }

    void feature_names_resolve_and_unknown_names_throw() {
        auto names = rhi_feature_names();
        CHECK(!names->empty());
        auto config = runtime_config_new();
        runtime_config_require_feature(*config, names->front());
        runtime_config_request_feature(*config, names->back());
        bool threw = false;
        try {
            runtime_config_require_feature(*config, "definitely-not-a-feature");
        } catch (const Error &error) {
            threw = std::string{error.what()}.find("definitely-not-a-feature") != std::string::npos;
        }
        CHECK(threw);
    }

    void camera_defaults_match_the_engine() {
        const CameraDesc member_defaults{};
        const CameraDesc engine_defaults = camera_desc_defaults();
        const auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-3f * std::fmax(1.0f, std::fabs(b)); };
        CHECK(member_defaults.projection == engine_defaults.projection);
        CHECK(near(member_defaults.vertical_fov_degrees, engine_defaults.vertical_fov_degrees));
        CHECK(near(member_defaults.orthographic_size, engine_defaults.orthographic_size));
        CHECK(near(member_defaults.aspect_ratio, engine_defaults.aspect_ratio));
        CHECK(near(member_defaults.near_clip, engine_defaults.near_clip));
        CHECK(near(member_defaults.far_clip, engine_defaults.far_clip));
        CHECK(near(member_defaults.focal_length_mm, engine_defaults.focal_length_mm));
        CHECK(near(member_defaults.aperture_f_stop, engine_defaults.aperture_f_stop));
        CHECK(near(member_defaults.shutter_seconds, engine_defaults.shutter_seconds));
        CHECK(near(member_defaults.iso, engine_defaults.iso));
        CHECK(near(member_defaults.focus_distance, engine_defaults.focus_distance));
        CHECK(member_defaults.aperture_blades == engine_defaults.aperture_blades);
        CHECK(member_defaults.culling_mask == engine_defaults.culling_mask);
        CHECK(member_defaults.active == engine_defaults.active);
        CHECK(near(member_defaults.render_scale, engine_defaults.render_scale));
        for (int i = 0; i < 4; ++i) {
            CHECK(near(member_defaults.clear_color[i], engine_defaults.clear_color[i]));
            CHECK(near(member_defaults.normalized_viewport[i], engine_defaults.normalized_viewport[i]));
            CHECK(near(member_defaults.orientation[i], engine_defaults.orientation[i]));
        }
        for (int i = 0; i < 2; ++i) {
            CHECK(near(member_defaults.sensor_size_mm[i], engine_defaults.sensor_size_mm[i]));
        }
    }

    void camera_round_trip_and_math() {
        CameraDesc desc{};
        desc.position[0] = 1.0f;
        desc.position[1] = 2.0f;
        desc.position[2] = 3.0f;
        desc.vertical_fov_degrees = 75.0f;
        auto camera = camera_new(desc);
        const CameraDesc back = camera_to_desc(*camera);
        CHECK(back.position[0] == 1.0f && back.position[1] == 2.0f && back.position[2] == 3.0f);
        CHECK(std::fabs(back.vertical_fov_degrees - 75.0f) < 1e-3f);

        camera_look_at(*camera, {1.0f, 2.0f, -10.0f}, {0.0f, 1.0f, 0.0f});
        const auto forward = camera_forward(*camera);
        CHECK(forward[2] < -0.99f);
        CHECK(camera_sees_point(*camera, {1.0f, 2.0f, -10.0f}));
        CHECK(!camera_sees_point(*camera, {1.0f, 2.0f, 50.0f}));
        const RayResult ray = camera_screen_ray(*camera, {50.0f, 50.0f}, {100.0f, 100.0f});
        CHECK(ray.ok && ray.ray.direction[2] < -0.99f);
    }

} // namespace

int main() {
    options_round_trip();
    feature_names_resolve_and_unknown_names_throw();
    camera_defaults_match_the_engine();
    camera_round_trip_and_math();
    std::printf("CxxApiRuntimeTest: all checks passed.\n");
    return 0;
}
