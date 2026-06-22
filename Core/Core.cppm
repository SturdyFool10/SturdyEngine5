module;

#include "Core/EngineBackend.hpp"
#include "Core/RenderSurface.hpp"
#include "Core/Renderer.hpp"

export module Sturdy.Core;

export import Sturdy.Foundation;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused"
#endif

export namespace SFT::Core {
    using ::SFT::Core::create_vulkan_backend;
    using ::SFT::Core::EngineBackend;
    using ::SFT::Core::Extent2D;
    using ::SFT::Core::FrameInput;
    using ::SFT::Core::renderer_error;
    using ::SFT::Core::RendererCapabilities;
    using ::SFT::Core::RendererError;
    using ::SFT::Core::RendererErrorCode;
    using ::SFT::Core::RendererExpected;
    using ::SFT::Core::RendererFeatureRequest;
    using ::SFT::Core::RendererInit;
    using ::SFT::Core::RendererResult;
    using ::SFT::Core::RenderSurfaceDescriptor;
    using ::SFT::Core::SurfaceSystem;
} // namespace SFT::Core

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
