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

    // Maps a reflected Slang stage onto its Vulkan stage bit. Returns 0 for stages that have no
    // direct VkShaderStageFlagBits (Unknown / Dispatch); callers treat 0 as "unmappable".
    [[nodiscard]] VkShaderStageFlagBits to_vk_shader_stage(Slang::ShaderStage stage) noexcept;

    // A single compiled SPIR-V entry point. The backend builds one of these per (source file, entry
    // point) pair. It carries the source file it came from, the entry point name and Vulkan stage,
    // and a shared handle to the full reflection of its source file — so pipeline construction can
    // read bindings/parameters without re-reflecting, and several entry points from one file share
    // a single reflection copy.
    class VulkanShaderModule {
      public:
        VulkanShaderModule() = default;
        ~VulkanShaderModule();

        VulkanShaderModule(const VulkanShaderModule &) = delete;
        VulkanShaderModule &operator=(const VulkanShaderModule &) = delete;

        VulkanShaderModule(VulkanShaderModule &&o) noexcept;
        VulkanShaderModule &operator=(VulkanShaderModule &&o) noexcept;

        // Takes SPIR-V as a span of 32-bit words (the natural format from glslc / slangc) plus the
        // provenance the backend keeps around: source file, entry point name + stage, and a shared
        // pointer to the source file's reflection.
        [[nodiscard]] static RendererExpected<VulkanShaderModule> create(
            VkDevice device,
            span<const u32> spirv,
            UString source_file,
            UString entry_point,
            VkShaderStageFlagBits stage,
            shared_ptr<const Slang::ShaderReflection> reflection) noexcept;

        // Build the pipeline stage create info for this module. Uses the stored stage and entry point;
        // pass overrides only when intentionally reusing the module's SPIR-V under a different stage.
        [[nodiscard]] VkPipelineShaderStageCreateInfo stage_info(
            const VkSpecializationInfo *specialization = nullptr) const noexcept;

        [[nodiscard]] VkShaderModule vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        // Borrowed views over the owned members. `ustr` can't be moved, but a prvalue built in the return
        // is elided into the caller — the one shape in which a borrowed return works.
        [[nodiscard]] ustr source_file() const noexcept;
        [[nodiscard]] ustr entry_point() const noexcept;
        [[nodiscard]] VkShaderStageFlagBits stage() const noexcept;

        // The full reflection of the source file this entry point was compiled from. Shared with
        // every other module compiled from the same file; never null for a module created via
        // create() with a valid reflection.
        [[nodiscard]] const shared_ptr<const Slang::ShaderReflection> &reflection() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkShaderModule module_ = VK_NULL_HANDLE;
        UString source_file_;
        UString entry_point_;
        VkShaderStageFlagBits stage_ = static_cast<VkShaderStageFlagBits>(0);
        shared_ptr<const Slang::ShaderReflection> reflection_;
    };

    // Identity for a compiled shader module: the source file it came from plus its entry point.
    // The backend keeps every VulkanShaderModule in a map under this key so a pipeline can ask for
    // "the vertex entry point of triangle.slang" directly.
    struct VulkanShaderModuleKey {
        UString source_file;
        UString entry_point;

        [[nodiscard]] bool operator==(const VulkanShaderModuleKey &) const = default;
    };

    // Hash functor for VulkanShaderModuleKey. Kept as a standalone functor (rather than a std::hash
    // specialization) so the map can be declared without exporting an addition to namespace std
    // across the module boundary.
    struct VulkanShaderModuleKeyHash {
        [[nodiscard]] std::size_t operator()(const VulkanShaderModuleKey &key) const noexcept;
    };

} // namespace SFT::Core::Vulkan
