/// Camera::project must not return a mirrored screen point for a world point behind the eye, and
/// Camera::operator== compares the description while ignoring temporal history.

#include <Engine/Camera.hpp>

#include <iostream>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
} // namespace

int main() {
    using SFT::Engine::Camera;

    Camera camera;
    camera.set_perspective(60.0f, 0.1f, 100.0f);
    camera.set_aspect_ratio(1.0f);
    camera.set_position({0.0f, 0.0f, 0.0f});
    camera.look_at({0.0f, 0.0f, -1.0f});

    const auto in_front = camera.project({0.0f, 0.0f, -5.0f}, {800.0f, 600.0f});
    check(in_front.has_value(), "a point in front of the camera must project");
    if (in_front) {
        check(std::abs(in_front->x - 400.0f) < 1.0f && std::abs(in_front->y - 300.0f) < 1.0f, "the view axis must project to the viewport centre");
    }
    check(!camera.project({0.0f, 0.0f, 5.0f}, {800.0f, 600.0f}).has_value(), "a point behind the camera must not project");
    check(!camera.project({1.0f, 1.0f, 5.0f}, {800.0f, 600.0f}).has_value(), "an off-axis point behind the camera must not project");

    Camera copy = camera;
    check(copy == camera, "a copy compares equal");
    copy.commit_frame();
    check(copy == camera, "temporal history must not affect equality");
    copy.set_position({1.0f, 0.0f, 0.0f});
    check(!(copy == camera), "a moved camera compares unequal");

    Camera moved;
    moved.set_previous_view_projection(camera.view_projection_matrix());
    check(moved.has_history(), "installing a previous view-projection must mark history present");
    check(moved.previous_view_projection_matrix() == camera.view_projection_matrix(), "the installed matrix must be what is reported");

    return failures == 0 ? 0 : 1;
}
