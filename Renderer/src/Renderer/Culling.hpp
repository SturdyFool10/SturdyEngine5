#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

using std::array;

namespace SFT::Renderer {


    struct Frustum {
        array<glm::vec4, 6> planes{};
    };

    /// Performs the frustum from view projection operation using the supplied arguments.
    ///
    /// @param view_projection `view_projection` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] Frustum frustum_from_view_projection(const glm::mat4 &view_projection) noexcept;


    /// Performs the frustum intersects sphere operation for `Renderer` using the supplied arguments.
    ///
    /// @param frustum `frustum` value used by the operation.
    /// @param center `center` value used by the operation.
    /// @param radius `radius` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool frustum_intersects_sphere(const Frustum &frustum, const glm::vec3 &center,
                                                        f32 radius) noexcept;

} // namespace SFT::Renderer
