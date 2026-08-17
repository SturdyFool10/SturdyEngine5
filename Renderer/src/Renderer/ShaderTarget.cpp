#include <Renderer/src/Renderer/ShaderTarget.hpp>


namespace SFT::Renderer {

    /// Performs the shader target for device operation for `Renderer` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
    Core::RendererExpected<RendererShaderTarget> shader_target_for_device(const RHI::RhiDevice &device) {
        switch (device.backend_type()) {
            case RHI::BackendType::Vulkan:
                return RendererShaderTarget{
                    .slang_target = {.format = Core::Slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_5"},
                    .module_language = RHI::ShaderLanguage::SpirV,
                };
            case RHI::BackendType::D3D12:
                return RendererShaderTarget{
                    .slang_target = {.format = Core::Slang::ShaderTargetFormat::Dxil, .profile = "sm_6_6"},
                    .module_language = RHI::ShaderLanguage::Dxil,
                };
            case RHI::BackendType::Metal:
            case RHI::BackendType::WebGpu:
                return std::unexpected(Core::GraphicsBackendError{
                    .code = Core::GraphicsBackendErrorCode::Unsupported,
                    .message = string{"Renderer shader modules are not supported for the active "} +
                               RHI::backend_type_name(device.backend_type()) + " backend.",
                });
        }

        return std::unexpected(Core::GraphicsBackendError{
            .code = Core::GraphicsBackendErrorCode::Unsupported,
            .message = "Renderer shader modules are not supported for an unknown RHI backend.",
        });
    }

    /// Performs the shader compile targets for device operation for `Renderer` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice &device) {
        if (device.backend_type() == RHI::BackendType::D3D12) {
            return {
                {.format = Core::Slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_5"},
                {.format = Core::Slang::ShaderTargetFormat::Dxil, .profile = "sm_6_6"},
            };
        }
        const auto target = shader_target_for_device(device);
        return target ? vector<Core::Slang::ShaderTarget>{target->slang_target}
                      : vector<Core::Slang::ShaderTarget>{};
    }

    /// Performs the shader compile targets for device operation for `Renderer` using the supplied arguments.
    ///
    /// @param device Device used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice *device) {
        return device != nullptr ? shader_compile_targets_for_device(*device)
                                 : vector<Core::Slang::ShaderTarget>{};
    }

} // namespace SFT::Renderer

