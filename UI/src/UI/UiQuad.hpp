#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::UI {

    enum class UiQuadKind : u32 { Rect = 0, Image = 1 };

                                                                                                       
                                                                                                
                                                                                                    
                                                                                                    
                                                                                                      
                                                                                                 
                                                                                                  
                                                                                                     
                                                                                                         
                                                                                       
       
                                                                                           
                                                                                                 
                                                                                                    
                                                                                                     
                                                                                                    
                                                                                                 
                                                    
    struct UiQuadInstance {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
                                                                                       
        glm::vec4 corner_radius{0.0f};
                                                                         
        glm::vec4 border_width{0.0f};
        glm::vec4 fill_color{1.0f};
        glm::vec4 border_color{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{1.0f};
        f32 kind = 0.0f;
        f32 _pad0 = 0.0f;
        f32 _pad1 = 0.0f;
        f32 _pad2 = 0.0f;
    };

} // namespace SFT::UI
