export module Sturdy.Core;

#pragma region Imports
export import Sturdy.Foundation;

export import :RenderSurface;
export import :RendererError;
export import :Licenses;
export import :Renderer;
export import :EngineBackend;
// Core/Vulkan is excluded from the Web build (see Core/CMakeLists.txt) — Vulkan is not one of
// Web's graphics APIs (see EngineBackend.cppm), so there is no :VulkanFeatures/:VulkanBackend
// partition to import there. A future WebGPU backend partition takes its place once it exists.
#if !defined(STURDY_PLATFORM_WEB)
export import :VulkanFeatures;
export import :VulkanBackend;
#endif
export import :Shader;
export import :ShaderDiscovery;
#pragma endregion
