#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/ScrollArea.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/TextEdit.hpp>

using std::vector;

                                                                                                      
                                                                                           
                                                                                                  
namespace SFT::UI {

    struct TextAreaResult {
        bool changed = false;
        bool focused = false;
                                                                                                        
                                             
        std::optional<ElementBounds> caret_bounds;
    };

                                                                                          
                                                                           
                                                                                                    
                                                                                                 
                                                                                                     
                                                                          
       
                                                                         
                                                                                                
                                                                                                  
                                                                                          
                                                                                                    
                                                                                                
       
                                                                                                      
                                                                                                  
                                                                                                     
                                                                                                  
                                
    [[nodiscard]] TextAreaResult text_area(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                   TextEditState &state, const TextEditInput &input, f32 delta_seconds,
                                                   const ScrollbarStyle &scrollbar_style, ScrollAreaState &scroll_state,
                                                   const UString &placeholder = {}, bool enabled = true);

} // namespace SFT::UI
