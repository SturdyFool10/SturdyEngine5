module;

#include "Core/EngineBackend.hpp"
#include "Core/RenderSurface.hpp"
#include "Core/Renderer.hpp"
#include "Core/Slang/Shader.hpp"

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
    using ::SFT::Core::invalid_render_surface_index;
    using ::SFT::Core::renderer_error;
    using ::SFT::Core::RendererCapabilities;
    using ::SFT::Core::RendererCreateInfo;
    using ::SFT::Core::RendererError;
    using ::SFT::Core::RendererErrorCode;
    using ::SFT::Core::RendererExpected;
    using ::SFT::Core::RendererFeatureRequest;
    using ::SFT::Core::RendererResult;
    using ::SFT::Core::RenderSurfaceCreateInfo;
    using ::SFT::Core::RenderSurfaceDescriptor;
    using ::SFT::Core::RenderSurfaceHandle;
    using ::SFT::Core::SurfaceProvider;
    using ::SFT::Core::SurfaceSystem;
} // namespace SFT::Core

export namespace SFT::Core::Slang {
    using ::SFT::Core::Slang::Shader;
    using ::SFT::Core::Slang::shader_error;
    using ::SFT::Core::Slang::shader_source_from_type;
    using ::SFT::Core::Slang::shader_unbounded_size;
    using ::SFT::Core::Slang::shader_unknown_size;
    using ::SFT::Core::Slang::ShaderBindingRangeReflection;
    using ::SFT::Core::Slang::ShaderBindingType;
    using ::SFT::Core::Slang::ShaderBytecode;
    using ::SFT::Core::Slang::ShaderCompileOptions;
    using ::SFT::Core::Slang::ShaderCompiler;
    using ::SFT::Core::Slang::ShaderDescriptorRangeReflection;
    using ::SFT::Core::Slang::ShaderDescriptorSetReflection;
    using ::SFT::Core::Slang::ShaderEntryPointReflection;
    using ::SFT::Core::Slang::ShaderEntryPointRequest;
    using ::SFT::Core::Slang::ShaderError;
    using ::SFT::Core::Slang::ShaderErrorCode;
    using ::SFT::Core::Slang::ShaderExpected;
    using ::SFT::Core::Slang::ShaderFieldReflection;
    using ::SFT::Core::Slang::ShaderMacro;
    using ::SFT::Core::Slang::ShaderMatrixLayout;
    using ::SFT::Core::Slang::ShaderParameterCategory;
    using ::SFT::Core::Slang::ShaderParameterReflection;
    using ::SFT::Core::Slang::ShaderReflection;
    using ::SFT::Core::Slang::ShaderResourceAccess;
    using ::SFT::Core::Slang::ShaderResourceShape;
    using ::SFT::Core::Slang::ShaderResult;
    using ::SFT::Core::Slang::ShaderScalarType;
    using ::SFT::Core::Slang::ShaderSource;
    using ::SFT::Core::Slang::ShaderSourceKind;
    using ::SFT::Core::Slang::ShaderStage;
    using ::SFT::Core::Slang::ShaderTarget;
    using ::SFT::Core::Slang::ShaderTargetFormat;
    using ::SFT::Core::Slang::ShaderTypeKind;
    using ::SFT::Core::Slang::ShaderTypeReflection;
    using ::SFT::Core::Slang::StaticShaderSource;
} // namespace SFT::Core::Slang

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
