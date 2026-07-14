#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/vec3.hpp>

#include <Core/RenderSurface.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Licenses.hpp>
#include <Core/Renderer.hpp>
#include <Core/EngineBackend.hpp>
// Core/Vulkan is excluded from the Web build (see Core/CMakeLists.txt) — Vulkan is not one of
// Web's graphics APIs (see EngineBackend.hpp), so there is no VulkanFeatures/VulkanBackend
// header to include there. A future WebGPU backend takes its place once it exists.
#if !defined(STURDY_PLATFORM_WEB)
#include <Core/Vulkan/VulkanFeatures.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
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
