#pragma once

#include <Foundation/src/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    using std::vector;

    // The renderer owns the policy that connects an active RHI backend to the bytecode Slang emits.
    // Keep this above both RHI backends and Core::Slang so neither lower layer depends on the other.
    struct RendererShaderTarget {
        Core::Slang::ShaderTarget slang_target;
        RHI::ShaderLanguage module_language = RHI::ShaderLanguage::SpirV;
    };


    [[nodiscard]] inline Core::RendererExpected<RendererShaderTarget> shader_target_for_device(const RHI::RhiDevice &device) {
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

    // DXIL needs canonical SPIR-V layout reflection as well as executable DXIL. Slang lowers
    // [[push_constant]] into HLSL cbuffer/root-signature terms in DXIL reflection, while the
    // SPIR-V layout retains the portable declaration translated by RHI into root constants.
    // SPIR-V intentionally comes first because ShaderCompiler stores reflection for target index 0.
    [[nodiscard]] inline vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
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

    [[nodiscard]] inline vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice *device) {
        return device != nullptr ? shader_compile_targets_for_device(*device)
                                 : vector<Core::Slang::ShaderTarget>{};
    }

} // namespace SFT::Renderer
