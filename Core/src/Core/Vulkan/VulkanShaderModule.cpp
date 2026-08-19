#include <Core/Vulkan/VulkanShaderModule.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Converts the supplied engine/RHI value to its Vulkan representation.
///
/// @param stage `stage` value used by the operation.
///
/// @return Returns the value converted to Vulkan shader stage representation.
/// @note This function does not throw exceptions.
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

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanShaderModule::~VulkanShaderModule() { destroy(); }

/// Performs the vulkan shader module operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanShaderModule::VulkanShaderModule(VulkanShaderModule &&o) noexcept
            : device_(o.device_),
              module_(o.module_),
              source_file_(std::move(o.source_file_)),
              entry_point_(std::move(o.entry_point_)),
              stage_(o.stage_),
              reflection_(std::move(o.reflection_)) {
            ZoneScopedN("VulkanShaderModule::VulkanShaderModule");
            o.device_ = VK_NULL_HANDLE;
            o.module_ = VK_NULL_HANDLE;
            o.stage_ = static_cast<VkShaderStageFlagBits>(0);
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanShaderModule &VulkanShaderModule::operator=(VulkanShaderModule &&o) noexcept {
            ZoneScopedN("VulkanShaderModule::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
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
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanShaderModule> VulkanShaderModule::create(
            VkDevice device,
            span<const u32> spirv,
            UString source_file,
            UString entry_point,
            VkShaderStageFlagBits stage,
            shared_ptr<const Slang::ShaderReflection> reflection) noexcept {
            ZoneScopedN("VulkanShaderModule::create");
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

/// Performs the stage info operation for `Vulkan` using the supplied arguments.
///
/// @param specialization `specialization` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineShaderStageCreateInfo VulkanShaderModule::stage_info(
            const VkSpecializationInfo *specialization) const noexcept {
            ZoneScopedN("VulkanShaderModule::stage_info");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkShaderModule VulkanShaderModule::vk_handle() const noexcept { return module_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanShaderModule::is_valid() const noexcept { return module_ != VK_NULL_HANDLE; }

/// Returns the current or globally available source file value.
///
/// @return Returns the current source file value.
/// @note This function does not throw exceptions.
[[nodiscard]] ustr VulkanShaderModule::source_file() const noexcept { return ustr{source_file_}; }

/// Returns the current or globally available entry point value.
///
/// @return Returns the current entry point value.
/// @note This function does not throw exceptions.
[[nodiscard]] ustr VulkanShaderModule::entry_point() const noexcept { return ustr{entry_point_}; }

/// Returns the current or globally available stage value.
///
/// @return Returns the current stage value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkShaderStageFlagBits VulkanShaderModule::stage() const noexcept { return stage_; }

/// Returns the current or globally available reflection value.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function does not throw exceptions.
[[nodiscard]] const shared_ptr<const Slang::ShaderReflection> &VulkanShaderModule::reflection() const noexcept {
            ZoneScopedN("VulkanShaderModule::reflection");
            return reflection_;
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanShaderModule::destroy() noexcept {
            ZoneScopedN("VulkanShaderModule::destroy");
            if (module_ == VK_NULL_HANDLE)
                return;
            vkDestroyShaderModule(device_, module_, nullptr);
            module_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            reflection_.reset();
        }

/// Invokes the callable behavior provided by `Vulkan`.
///
/// @param key Key used to identify the requested entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] std::size_t VulkanShaderModuleKeyHash::operator()(const VulkanShaderModuleKey &key) const noexcept {
            ZoneScopedN("VulkanShaderModuleKeyHash::operator");
            const std::size_t h1 = std::hash<UString>{}(key.source_file);
            const std::size_t h2 = std::hash<UString>{}(key.entry_point);

            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }

} // namespace SFT::Core::Vulkan
