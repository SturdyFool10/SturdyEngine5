#pragma once

#include <Foundation/Foundation.hpp>

#include <Core/Core.hpp>

#include <Renderer/Handles.hpp>

namespace SFT::Renderer {

    using OffscreenRenderTargetHandle = Handle<struct OffscreenRenderTargetTag>;

                                                                                                 
                                                                                                          
                                                                                                    
                                                                                                  
    struct OffscreenRenderTargetDescription {
        Core::Extent2D extent{};
        UString label;
    };

} // namespace SFT::Renderer
