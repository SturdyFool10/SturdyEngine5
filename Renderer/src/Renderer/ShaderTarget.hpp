#pragma once

#include <Foundation/src/Foundation.hpp>

#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    using std::vector;

    /// The renderer owns the policy that connects an active RHI backend to the bytecode Slang emits.
    /// Keep this above both RHI backends and Core::Slang so neither lower layer depends on the other.
    struct RendererShaderTarget {
        Core::Slang::ShaderTarget slang_target;
        RHI::ShaderLanguage module_language = RHI::ShaderLanguage::SpirV;
    };


    [[nodiscard]] Core::RendererExpected<RendererShaderTarget> shader_target_for_device(const RHI::RhiDevice &device);

    /// DXIL needs canonical SPIR-V layout reflection as well as executable DXIL. Slang lowers
    /// [[push_constant]] into HLSL cbuffer/root-signature terms in DXIL reflection, while the
    /// SPIR-V layout retains the portable declaration translated by RHI into root constants.
    /// SPIR-V intentionally comes first because ShaderCompiler stores reflection for target index 0.
    [[nodiscard]] vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice &device);

    [[nodiscard]] vector<Core::Slang::ShaderTarget> shader_compile_targets_for_device(
        const RHI::RhiDevice *device);

} // namespace SFT::Renderer
