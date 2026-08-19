#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Slang/ShaderReflection.hpp>
#include <Core/Slang/ShaderTypes.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::shared_ptr;
using std::span;

namespace SFT::Core::Vulkan {


    /// Converts the supplied engine/RHI value to its Vulkan representation.
    ///
    /// @param stage `stage` value used by the operation.
    ///
    /// @return Returns the value converted to Vulkan shader stage representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] VkShaderStageFlagBits to_vk_shader_stage(Slang::ShaderStage stage) noexcept;


    class VulkanShaderModule {
      public:
        /// Constructs a `VulkanShaderModule` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanShaderModule() = default;
        /// Destroys the `VulkanShaderModule` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanShaderModule();

        /// Disables this construction form for `VulkanShaderModule`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanShaderModule(const VulkanShaderModule &) = delete;
        /// Assigns a new value to this `VulkanShaderModule`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanShaderModule &operator=(const VulkanShaderModule &) = delete;

        /// Constructs a `VulkanShaderModule` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanShaderModule(VulkanShaderModule &&o) noexcept;
        /// Assigns a new value to this `VulkanShaderModule`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanShaderModule &operator=(VulkanShaderModule &&o) noexcept;


        /// Creates a `VulkanShaderModule` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param spirv `spirv` value used by the operation.
        /// @param source_file `source_file` value used by the operation.
        /// @param entry_point `entry_point` value used by the operation.
        /// @param stage `stage` value used by the operation.
        /// @param reflection `reflection` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanShaderModule> create(
            VkDevice device,
            span<const u32> spirv,
            UString source_file,
            UString entry_point,
            VkShaderStageFlagBits stage,
            shared_ptr<const Slang::ShaderReflection> reflection) noexcept;


        /// Performs the stage info operation for `VulkanShaderModule` using the supplied arguments.
        ///
        /// @param specialization `specialization` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineShaderStageCreateInfo stage_info(
            const VkSpecializationInfo *specialization = nullptr) const noexcept;

        /// Returns the Vulkan handle associated with this `VulkanShaderModule`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkShaderModule vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanShaderModule`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;


        /// Returns the current or globally available source file value.
        ///
        /// @return Returns the current source file value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ustr source_file() const noexcept;
        /// Returns the current or globally available entry point value.
        ///
        /// @return Returns the current entry point value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ustr entry_point() const noexcept;
        /// Returns the current or globally available stage value.
        ///
        /// @return Returns the current stage value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkShaderStageFlagBits stage() const noexcept;


        /// Returns the current or globally available reflection value.
        ///
        /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const shared_ptr<const Slang::ShaderReflection> &reflection() const noexcept;

        /// Destroys or releases the `VulkanShaderModule` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkShaderModule module_ = VK_NULL_HANDLE;
        UString source_file_;
        UString entry_point_;
        VkShaderStageFlagBits stage_ = static_cast<VkShaderStageFlagBits>(0);
        shared_ptr<const Slang::ShaderReflection> reflection_;
    };


    struct VulkanShaderModuleKey {
        UString source_file;
        UString entry_point;

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool operator==(const VulkanShaderModuleKey &) const = default;
    };


    struct VulkanShaderModuleKeyHash {
        /// Invokes the callable behavior provided by `VulkanShaderModuleKeyHash`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::size_t operator()(const VulkanShaderModuleKey &key) const noexcept;
    };

} // namespace SFT::Core::Vulkan
