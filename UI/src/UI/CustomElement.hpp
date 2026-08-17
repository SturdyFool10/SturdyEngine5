#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <glm/vec2.hpp>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

#include "Style.hpp"

                                                                                    
                                                                                                     
                                                                                                   
                     
namespace SFT::UI {

                                                                                                    
                                                                                                      
                                                                                
                                                                                                      
                                                                                                   
                                                                                                      
                                                                                                     
                                                                                                    
                                                                                                    
                                                                                                   
                                                                                      
    struct UiElementConstants {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        glm::vec2 viewport_size{0.0f};
        f32 _reserved0 = 0.0f;
        f32 _reserved1 = 0.0f;
    };

                                                                                                    
                                                                                            
                                                                                                  
                                                                                                
                                                                                                   
                                                             
       
                                                                                                      
                                                                                                      
                                                                                                   
                                                                    
    struct CustomShaderRef {
        std::string shader_path;
        std::string module_name;
        std::string fragment_entry_point = "fragmentMain";
                                                                                                 
                                                                                                      
                                                                                                 
                                                                                                      
           
                                                                                                       
                                                                                                  
                                                                                                    
                                                                                                    
                                                                                                       
                                                                               
                                                                                                    
                                                                                              
                                   
        std::vector<std::byte> push_constants;
    };

                                                                                               
                                                                                                  
                                                                                                   
                                                                                                    
                                                                  
    struct CustomDraw {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        const CustomShaderRef *shader = nullptr;
        RHI::Rect2D scissor{};
        PaintKey paint{};
    };

} // namespace SFT::UI
