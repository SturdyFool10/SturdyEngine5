#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#pragma endregion

namespace SFT::WindowManager {

    using WindowExtent = glm::u32vec2;
    using WindowPosition = glm::i32vec2;

                                                                                                     
                                                                                                        
                                                                                                   
                                                                                                   
                                                                                                
                                                                         
    struct TextInputArea {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 cursor_offset_x = 0.0f;
    };

} // namespace SFT::WindowManager
