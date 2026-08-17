#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::Renderer {

                                                                                                     
                                                                               
    struct GeometryVertex {
        glm::vec3 position{};
        glm::vec3 normal{};
        glm::vec2 uv{};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
                                                                                                     
                                                                                                     
                                                                                                       
                                                                                                      
                                                                                               
        glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    };

} // namespace SFT::Renderer
