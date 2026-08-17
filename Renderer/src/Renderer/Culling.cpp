#include <Renderer/src/Renderer/Culling.hpp>


namespace SFT::Renderer {

    /// Performs the frustum from view projection operation for `Renderer` using the supplied arguments.
    ///
    /// @param view_projection `view_projection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    Frustum frustum_from_view_projection(const glm::mat4 &view_projection) noexcept {
        const glm::mat4 &m = view_projection;
        Frustum frustum{};

        auto row = [&m](int r) { return glm::vec4{m[0][r], m[1][r], m[2][r], m[3][r]}; };
        const glm::vec4 r0 = row(0);
        const glm::vec4 r1 = row(1);
        const glm::vec4 r2 = row(2);
        const glm::vec4 r3 = row(3);
        frustum.planes[0] = r3 + r0;
        frustum.planes[1] = r3 - r0;
        frustum.planes[2] = r3 + r1;
        frustum.planes[3] = r3 - r1;
        frustum.planes[4] = r3 + r2;
        frustum.planes[5] = r3 - r2;
        for (glm::vec4 &plane : frustum.planes) {
            const f32 length = glm::length(glm::vec3{plane});
            if (length > 1e-8f) {
                plane /= length;
            }
        }
        return frustum;
    }

    /// Performs the frustum intersects sphere operation for `Renderer` using the supplied arguments.
    ///
    /// @param frustum `frustum` value used by the operation.
    /// @param center `center` value used by the operation.
    /// @param radius `radius` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool frustum_intersects_sphere(const Frustum &frustum, const glm::vec3 &center,
                                                        f32 radius) noexcept {
        for (const glm::vec4 &plane : frustum.planes) {
            if (glm::dot(glm::vec3{plane}, center) + plane.w < -radius) {
                return false;
            }
        }
        return true;
    }

} // namespace SFT::Renderer

