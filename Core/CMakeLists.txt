sturdy_add_package(Core
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/EngineBackend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/EngineBackend.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan/VulkanBackend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan/VulkanBackend.hpp"

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::VMA
        Sturdy::Volk
        Sturdy::GLM
        Sturdy::SDL3
        Sturdy::Vulkan
        Sturdy::Slang

    DEBUG_PUBLIC_DEPS
)
