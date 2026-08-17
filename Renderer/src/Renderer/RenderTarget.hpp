#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Core/Core.hpp>

#include "Handles.hpp"

namespace SFT::Renderer {

    using OffscreenRenderTargetHandle = Handle<struct OffscreenRenderTargetTag>;

                                                                                                 
                                                                                                          
                                                                                                    
                                                                                                  
    struct OffscreenRenderTargetDescription {
        Core::Extent2D extent{};
        UString label;
    };

} // namespace SFT::Renderer
