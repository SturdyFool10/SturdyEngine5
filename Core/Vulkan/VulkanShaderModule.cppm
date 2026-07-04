module;
#pragma region Imports
#include "volk.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#pragma endregion

export module Sturdy.Core:VulkanShaderModule;

#pragma region Imports
import :RendererError;
import :ShaderReflection;
import :ShaderTypes;
import Sturdy.Foundation;
#pragma endregion

using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using std::shared_ptr;
using std::span;

export namespace SFT::Core::Vulkan {

    // Maps a reflected Slang stage onto its Vulkan stage bit. Returns 0 for stages that have no
    // direct VkShaderStageFlagBits (Unknown / Dispatch); callers treat 0 as "unmappable".
    [[nodiscard]] inline VkShaderStageFlagBits to_vk_shader_stage(Slang::ShaderStage stage) noexcept {
        using Slang::ShaderStage;
        switch (stage) {
            case ShaderStage::Vertex:
                return VK_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::Hull:
                return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case ShaderStage::Domain:
                return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            case ShaderStage::Geometry:
                return VK_SHADER_STAGE_GEOMETRY_BIT;
            case ShaderStage::Fragment:
                return VK_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::Compute:
                return VK_SHADER_STAGE_COMPUTE_BIT;
            case ShaderStage::RayGeneration:
                return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case ShaderStage::Intersection:
                return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case ShaderStage::AnyHit:
                return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case ShaderStage::ClosestHit:
                return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case ShaderStage::Miss:
                return VK_SHADER_STAGE_MISS_BIT_KHR;
            case ShaderStage::Callable:
                return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            case ShaderStage::Mesh:
                return VK_SHADER_STAGE_MESH_BIT_EXT;
            case ShaderStage::Amplification:
                return VK_SHADER_STAGE_TASK_BIT_EXT;
            case ShaderStage::Dispatch:
            case ShaderStage::Unknown:
                break;
        }
        return static_cast<VkShaderStageFlagBits>(0);
    }

    // A single compiled SPIR-V entry point. The backend builds one of these per (source file, entry
    // point) pair. It carries the source file it came from, the entry point name and Vulkan stage,
    // and a shared handle to the full reflection of its source file — so pipeline construction can
    // read bindings/parameters without re-reflecting, and several entry points from one file share
    // a single reflection copy.
    class VulkanShaderModule {
      public:
        VulkanShaderModule() = default;
        ~VulkanShaderModule() { destroy(); }

        VulkanShaderModule(const VulkanShaderModule &) = delete;
        VulkanShaderModule &operator=(const VulkanShaderModule &) = delete;

        VulkanShaderModule(VulkanShaderModule &&o) noexcept
            : device_(o.device_),
              module_(o.module_),
              source_file_(std::move(o.source_file_)),
              entry_point_(std::move(o.entry_point_)),
              stage_(o.stage_),
              reflection_(std::move(o.reflection_)) {
            o.device_ = VK_NULL_HANDLE;
            o.module_ = VK_NULL_HANDLE;
            o.stage_ = static_cast<VkShaderStageFlagBits>(0);
        }
        VulkanShaderModule &operator=(VulkanShaderModule &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                module_ = o.module_;
                source_file_ = std::move(o.source_file_);
                entry_point_ = std::move(o.entry_point_);
                stage_ = o.stage_;
                reflection_ = std::move(o.reflection_);
                o.device_ = VK_NULL_HANDLE;
                o.module_ = VK_NULL_HANDLE;
                o.stage_ = static_cast<VkShaderStageFlagBits>(0);
            }
            return *this;
        }

        // Takes SPIR-V as a span of 32-bit words (the natural format from glslc / slangc) plus the
        // provenance the backend keeps around: source file, entry point name + stage, and a shared
        // pointer to the source file's reflection.
        [[nodiscard]] static RendererExpected<VulkanShaderModule> create(
            VkDevice device,
            span<const u32> spirv,
            UString source_file,
            UString entry_point,
            VkShaderStageFlagBits stage,
            shared_ptr<const Slang::ShaderReflection> reflection) noexcept {
            VkShaderModuleCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .codeSize = spirv.size_bytes(),
                .pCode = spirv.data(),
            };
            VkShaderModule mod = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateShaderModule failed.");
            VulkanShaderModule out;
            out.device_ = device;
            out.module_ = mod;
            out.source_file_ = std::move(source_file);
            out.entry_point_ = std::move(entry_point);
            out.stage_ = stage;
            out.reflection_ = std::move(reflection);
            return out;
        }

        // Build the pipeline stage create info for this module. Uses the stored stage and entry point;
        // pass overrides only when intentionally reusing the module's SPIR-V under a different stage.
        [[nodiscard]] VkPipelineShaderStageCreateInfo stage_info(
            const VkSpecializationInfo *specialization = nullptr) const noexcept {
            return {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = stage_,
                .module = module_,
                .pName = entry_point_.c_str(),
                .pSpecializationInfo = specialization,
            };
        }

        [[nodiscard]] VkShaderModule vk_handle() const noexcept { return module_; }
        [[nodiscard]] bool is_valid() const noexcept { return module_ != VK_NULL_HANDLE; }
        // Borrowed views over the owned members. `ustr` can't be moved, but a prvalue built in the return
        // is elided into the caller — the one shape in which a borrowed return works.
        [[nodiscard]] ustr source_file() const noexcept { return ustr{source_file_}; }
        [[nodiscard]] ustr entry_point() const noexcept { return ustr{entry_point_}; }
        [[nodiscard]] VkShaderStageFlagBits stage() const noexcept { return stage_; }

        // The full reflection of the source file this entry point was compiled from. Shared with
        // every other module compiled from the same file; never null for a module created via
        // create() with a valid reflection.
        [[nodiscard]] const shared_ptr<const Slang::ShaderReflection> &reflection() const noexcept {
            return reflection_;
        }

        void destroy() noexcept {
            if (module_ == VK_NULL_HANDLE)
                return;
            vkDestroyShaderModule(device_, module_, nullptr);
            module_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            reflection_.reset();
        }

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
        [[nodiscard]] std::size_t operator()(const VulkanShaderModuleKey &key) const noexcept {
            const std::size_t h1 = std::hash<UString>{}(key.source_file);
            const std::size_t h2 = std::hash<UString>{}(key.entry_point);
            // 0x9e3779b97f4a7c15 is the 64-bit golden-ratio constant; the usual hash_combine mix.
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };

} // namespace SFT::Core::Vulkan
