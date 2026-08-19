#pragma once

#include <Foundation/Foundation.hpp>

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/TextEdit.hpp>

                                                                                                 
                                                               
namespace SFT::UI {

    struct TextInputResult {
        bool changed = false;
                                                                                            
                                                                                                    
                             
        bool submitted = false;
        bool focused = false;
                                                                                     
                                                                                                      
                                                                                                  
                                                                                                 
                                                                                                 
                                                                     
        std::optional<ElementBounds> caret_bounds;
    };

                                                                                                    
                                                                                         
                                                                                                   
                                                                                               
              
       
                                                                                                     
                                                                                                
                                                                                                    
                                                                                                 
                                                                                                 
                                                                                                
    [[nodiscard]] TextInputResult text_input(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                     TextEditState &state, const TextEditInput &input,
                                                     f32 delta_seconds, const UString &placeholder = {},
                                                     bool enabled = true);

} // namespace SFT::UI
