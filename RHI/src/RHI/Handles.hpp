#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::RHI {


    template <class Tag>
    struct Handle {
        u64 value = 0;

        /// Reports whether valid holds for this `Handle`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        /// Converts the `Handle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(Handle, Handle) noexcept = default;
    };


    using BufferHandle = Handle<struct BufferTag>;
    using TextureHandle = Handle<struct TextureTag>;
    using TextureViewHandle = Handle<struct TextureViewTag>;
    using SamplerHandle = Handle<struct SamplerTag>;
    using ShaderModuleHandle = Handle<struct ShaderModuleTag>;
    using BindGroupLayoutHandle = Handle<struct BindGroupLayoutTag>;
    using BindGroupHandle = Handle<struct BindGroupTag>;
    using PipelineLayoutHandle = Handle<struct PipelineLayoutTag>;
    using RenderPipelineHandle = Handle<struct RenderPipelineTag>;
    using ComputePipelineHandle = Handle<struct ComputePipelineTag>;
    using RayTracingPipelineHandle = Handle<struct RayTracingPipelineTag>;
    using AccelerationStructureHandle = Handle<struct AccelerationStructureTag>;
    using CommandBufferHandle = Handle<struct CommandBufferTag>;
    using RenderBundleHandle = Handle<struct RenderBundleTag>;
    using SurfaceHandle = Handle<struct SurfaceTag>;
    using SwapchainHandle = Handle<struct SwapchainTag>;
    using SemaphoreHandle = Handle<struct SemaphoreTag>;
    using FenceHandle = Handle<struct FenceTag>;
    using QuerySetHandle = Handle<struct QuerySetTag>;

} // namespace SFT::RHI
