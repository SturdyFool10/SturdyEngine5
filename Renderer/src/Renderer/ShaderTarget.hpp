#pragma once

#include <Foundation/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    using std::vector;


    struct RendererShaderTarget {
        Core::Slang::ShaderTarget slang_target;
        RHI::ShaderLanguage module_language = RHI::ShaderLanguage::SpirV;
    };


    /// Performs the shader target for device operation using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
    [[nodiscard]] Core::RendererExpected<RendererShaderTarget> shader_target_for_device(const RHI::RhiDevice &device);


    /// Performs the shader compile targets for device operation using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice &device);

    /// Performs the shader compile targets for device operation using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice *device);

} // namespace SFT::Renderer
