#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec3.hpp>
#pragma endregion

namespace SFT::Renderer {

                                                                                                   
                                            
    struct DirectionalLight {
        glm::vec3 direction{0.35f, -0.75f, 0.55f};
        glm::vec3 radiance{4.0f, 3.75f, 3.35f};
                                                                                                    
                                                                                                     
        f32 angular_radius_degrees = 0.27f;
        bool casts_shadows = true;
    };

                                                                                                   
                                                                                                     
                                                           
    struct SpotLight {
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 inner_cone_cos = 0.97f;
        f32 outer_cone_cos = 0.90f;
                                                                                                   
                                                                                
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

                                       
    struct PointLight {
        glm::vec3 position{0.0f};
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

} // namespace SFT::Renderer
