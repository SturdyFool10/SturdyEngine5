#pragma once

#include <Foundation/src/Foundation.hpp>

#include <glm/vec3.hpp>

#include <Core/RenderSurface.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Licenses.hpp>
#include <Core/Renderer.hpp>
#include <Core/EngineBackend.hpp>



#if !defined(STURDY_PLATFORM_WEB)
#include <Core/Vulkan/VulkanFeatures.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#endif
#if defined(_WIN32)
#include <Core/D3D12/D3D12Backend.hpp>
#endif
#include <Core/Slang/Shader.hpp>
#include <Core/Slang/ShaderDiscovery.hpp>
#include <Core/Slang/ShaderVariant.hpp>
#include <Core/Slang/ShaderWatcher.hpp>

namespace SFT::Core {
    struct Triangle {
        glm::vec3 a;
        glm::vec3 b;
        glm::vec3 c;
    };
}
