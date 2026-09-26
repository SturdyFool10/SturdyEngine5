#pragma once

#include <Foundation/Foundation.hpp>

#include <string>
#include <string_view>

#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    /// A compute shader entry point the renderer compiles once and caches. Identity is
    /// (`shader_path`, `module_name`, `entry_point`).
    ///
    /// The shader may declare any number of resources in descriptor set 0 (sampled textures, storage
    /// textures, samplers) and one push-constant block. Textures are bound by their declared name at
    /// record time; every sampler is bound to a shared linear/clamp-to-edge sampler automatically.
    struct ComputeKernelDescription {
        std::string shader_path;
        std::string module_name;
        std::string entry_point = "computeMain";
        std::string label;
    };

    /// Handle to a prepared kernel; valid until the renderer's graphics resources are torn down.
    struct ComputeKernelId {
        u32 index = ~0u;
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return index != ~0u; }
    };

    /// A texture supplied for a kernel resource, by the name it has in the shader.
    struct ComputeBinding {
        std::string_view name;
        RHI::TextureViewHandle view;
    };

} // namespace SFT::Renderer
