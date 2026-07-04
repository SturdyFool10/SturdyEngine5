module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanDescriptors;

#pragma region Imports
import :RendererError;
import Sturdy.Foundation;
#pragma endregion

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;
using std::vector;

export namespace SFT::Core::Vulkan {

    // ─── VulkanDescriptorSetLayout ───────────────────────────────────────────────

    class VulkanDescriptorSetLayout {
      public:
        VulkanDescriptorSetLayout() = default;
        ~VulkanDescriptorSetLayout() { destroy(); }

        VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout &) = delete;
        VulkanDescriptorSetLayout &operator=(const VulkanDescriptorSetLayout &) = delete;

        VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }
        VulkanDescriptorSetLayout &operator=(VulkanDescriptorSetLayout &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create(
            VkDevice device,
            const VkDescriptorSetLayoutCreateInfo &info) noexcept {
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateDescriptorSetLayout failed.");
            VulkanDescriptorSetLayout out;
            out.device_ = device;
            out.layout_ = layout;
            return out;
        }

        // Convenience: build from a flat list of bindings.
        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create_from_bindings(
            VkDevice device,
            span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayoutCreateFlags flags = 0) noexcept {
            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .bindingCount = static_cast<u32>(bindings.size()),
                .pBindings = bindings.data(),
            };
            return create(device, info);
        }

        [[nodiscard]] VkDescriptorSetLayout vk_handle() const noexcept { return layout_; }
        [[nodiscard]] bool is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

        // Returns whether a descriptor set with this layout can be created on the device.
        [[nodiscard]] VkDescriptorSetLayoutSupport support(const VkDescriptorSetLayoutCreateInfo &info) const noexcept {
            VkDescriptorSetLayoutSupport s{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = nullptr};
            vkGetDescriptorSetLayoutSupport(device_, &info, &s);
            return s;
        }

        void destroy() noexcept {
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    };

    // ─── VulkanDescriptorPool ────────────────────────────────────────────────────

    class VulkanDescriptorPool {
      public:
        VulkanDescriptorPool() = default;
        ~VulkanDescriptorPool() { destroy(); }

        VulkanDescriptorPool(const VulkanDescriptorPool &) = delete;
        VulkanDescriptorPool &operator=(const VulkanDescriptorPool &) = delete;

        VulkanDescriptorPool(VulkanDescriptorPool &&o) noexcept
            : device_(o.device_), pool_(o.pool_) {
            o.device_ = VK_NULL_HANDLE;
            o.pool_ = VK_NULL_HANDLE;
        }
        VulkanDescriptorPool &operator=(VulkanDescriptorPool &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pool_ = o.pool_;
                o.device_ = VK_NULL_HANDLE;
                o.pool_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create(
            VkDevice device,
            const VkDescriptorPoolCreateInfo &info) noexcept {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateDescriptorPool failed.");
            VulkanDescriptorPool out;
            out.device_ = device;
            out.pool_ = pool;
            return out;
        }

        // Convenience: create a pool from a flat list of type/count pairs.
        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create_from_sizes(
            VkDevice device,
            span<const VkDescriptorPoolSize> sizes,
            u32 max_sets,
            VkDescriptorPoolCreateFlags flags = 0) noexcept {
            VkDescriptorPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .maxSets = max_sets,
                .poolSizeCount = static_cast<u32>(sizes.size()),
                .pPoolSizes = sizes.data(),
            };
            return create(device, info);
        }

        [[nodiscard]] VkDescriptorPool vk_handle() const noexcept { return pool_; }
        [[nodiscard]] bool is_valid() const noexcept { return pool_ != VK_NULL_HANDLE; }

        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate(
            span<const VkDescriptorSetLayout> layouts) const {
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = pool_,
                .descriptorSetCount = static_cast<u32>(layouts.size()),
                .pSetLayouts = layouts.data(),
            };
            vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
            if (vkAllocateDescriptorSets(device_, &info, sets.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return sets;
        }

        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout) const noexcept {
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorPool = pool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return set;
        }

        [[nodiscard]] RendererResult free(span<const VkDescriptorSet> sets) noexcept {
            if (vkFreeDescriptorSets(device_, pool_, static_cast<u32>(sets.size()), sets.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkFreeDescriptorSets failed.");
            return {};
        }

        // Recycles all descriptor sets allocated from this pool back to the pool.
        [[nodiscard]] RendererResult reset(VkDescriptorPoolResetFlags flags = 0) noexcept {
            if (vkResetDescriptorPool(device_, pool_, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetDescriptorPool failed.");
            return {};
        }

        void destroy() noexcept {
            if (pool_ == VK_NULL_HANDLE)
                return;
            vkDestroyDescriptorPool(device_, pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
