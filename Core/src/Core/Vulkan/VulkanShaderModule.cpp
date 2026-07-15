#include "VulkanShaderModule.hpp"

namespace SFT::Core::Vulkan {

VkShaderStageFlagBits to_vk_shader_stage(Slang::ShaderStage stage) noexcept {
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

VulkanShaderModule::~VulkanShaderModule() { destroy(); }

VulkanShaderModule::VulkanShaderModule(VulkanShaderModule &&o) noexcept
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

VulkanShaderModule &VulkanShaderModule::operator=(VulkanShaderModule &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanShaderModule> VulkanShaderModule::create(
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
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateShaderModule failed.");
            VulkanShaderModule out;
            out.device_ = device;
            out.module_ = mod;
            out.source_file_ = std::move(source_file);
            out.entry_point_ = std::move(entry_point);
            out.stage_ = stage;
            out.reflection_ = std::move(reflection);
            return out;
        }

[[nodiscard]] VkPipelineShaderStageCreateInfo VulkanShaderModule::stage_info(
            const VkSpecializationInfo *specialization) const noexcept {
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

[[nodiscard]] VkShaderModule VulkanShaderModule::vk_handle() const noexcept { return module_; }

[[nodiscard]] bool VulkanShaderModule::is_valid() const noexcept { return module_ != VK_NULL_HANDLE; }

[[nodiscard]] ustr VulkanShaderModule::source_file() const noexcept { return ustr{source_file_}; }

[[nodiscard]] ustr VulkanShaderModule::entry_point() const noexcept { return ustr{entry_point_}; }

[[nodiscard]] VkShaderStageFlagBits VulkanShaderModule::stage() const noexcept { return stage_; }

[[nodiscard]] const shared_ptr<const Slang::ShaderReflection> &VulkanShaderModule::reflection() const noexcept {
            return reflection_;
        }

void VulkanShaderModule::destroy() noexcept {
            if (module_ == VK_NULL_HANDLE)
                return;
            vkDestroyShaderModule(device_, module_, nullptr);
            module_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            reflection_.reset();
        }

[[nodiscard]] std::size_t VulkanShaderModuleKeyHash::operator()(const VulkanShaderModuleKey &key) const noexcept {
            const std::size_t h1 = std::hash<UString>{}(key.source_file);
            const std::size_t h2 = std::hash<UString>{}(key.entry_point);
            // 0x9e3779b97f4a7c15 is the 64-bit golden-ratio constant; the usual hash_combine mix.
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }

} // namespace SFT::Core::Vulkan
