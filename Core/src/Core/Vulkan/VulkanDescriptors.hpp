#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    // ─── VulkanDescriptorSetLayout ───────────────────────────────────────────────

    class VulkanDescriptorSetLayout {
      public:
        VulkanDescriptorSetLayout() = default;
        ~VulkanDescriptorSetLayout();

        VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout &) = delete;
        VulkanDescriptorSetLayout &operator=(const VulkanDescriptorSetLayout &) = delete;

        VulkanDescriptorSetLayout(VulkanDescriptorSetLayout &&o) noexcept;
        VulkanDescriptorSetLayout &operator=(VulkanDescriptorSetLayout &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create(
            VkDevice device,
            const VkDescriptorSetLayoutCreateInfo &info) noexcept;

        // Convenience: build from a flat list of bindings.
        [[nodiscard]] static RendererExpected<VulkanDescriptorSetLayout> create_from_bindings(
            VkDevice device,
            span<const VkDescriptorSetLayoutBinding> bindings,
            VkDescriptorSetLayoutCreateFlags flags = 0) noexcept;

        [[nodiscard]] VkDescriptorSetLayout vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        // Returns whether a descriptor set with this layout can be created on the device.
        [[nodiscard]] VkDescriptorSetLayoutSupport support(const VkDescriptorSetLayoutCreateInfo &info) const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    };

    class DescriptorSetLayoutBuilder {
      public:
        DescriptorSetLayoutBuilder &add_binding(u32 binding,
                                                VkDescriptorType type,
                                                VkShaderStageFlags stages,
                                                u32 count = 1,
                                                const VkSampler *immutable_samplers = nullptr);

        DescriptorSetLayoutBuilder &set_last_binding_flags(VkDescriptorBindingFlags flags) noexcept;

        DescriptorSetLayoutBuilder &set_flags(VkDescriptorSetLayoutCreateFlags flags) noexcept;

        [[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> create(VkDevice device) const noexcept;

        [[nodiscard]] span<const VkDescriptorSetLayoutBinding> bindings() const noexcept;
        [[nodiscard]] span<const VkDescriptorBindingFlags> binding_flags() const noexcept;

      private:
        vector<VkDescriptorSetLayoutBinding> bindings_;
        vector<VkDescriptorBindingFlags> binding_flags_;
        VkDescriptorSetLayoutCreateFlags flags_ = 0;
    };

    // ─── VulkanDescriptorPool ────────────────────────────────────────────────────

    class VulkanDescriptorPool {
      public:
        VulkanDescriptorPool() = default;
        ~VulkanDescriptorPool();

        VulkanDescriptorPool(const VulkanDescriptorPool &) = delete;
        VulkanDescriptorPool &operator=(const VulkanDescriptorPool &) = delete;

        VulkanDescriptorPool(VulkanDescriptorPool &&o) noexcept;
        VulkanDescriptorPool &operator=(VulkanDescriptorPool &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create(
            VkDevice device,
            const VkDescriptorPoolCreateInfo &info) noexcept;

        // Convenience: create a pool from a flat list of type/count pairs.
        [[nodiscard]] static RendererExpected<VulkanDescriptorPool> create_from_sizes(
            VkDevice device,
            span<const VkDescriptorPoolSize> sizes,
            u32 max_sets,
            VkDescriptorPoolCreateFlags flags = 0) noexcept;

        [[nodiscard]] VkDescriptorPool vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate(
            span<const VkDescriptorSetLayout> layouts) const;

        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout) const noexcept;

        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout,
            u32 variable_descriptor_count) const noexcept;

        [[nodiscard]] RendererResult free(span<const VkDescriptorSet> sets) noexcept;

        // Recycles all descriptor sets allocated from this pool back to the pool.
        [[nodiscard]] RendererResult reset(VkDescriptorPoolResetFlags flags = 0) noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
    };

    class VulkanDescriptorSet {
      public:
        VulkanDescriptorSet() = default;
        explicit VulkanDescriptorSet(VkDescriptorSet set) noexcept;

        [[nodiscard]] VkDescriptorSet vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        VkDescriptorSet set_ = VK_NULL_HANDLE;
    };

    class DescriptorSetWriter {
      public:
        DescriptorSetWriter &set_descriptor_set(VkDescriptorSet set) noexcept;

        DescriptorSetWriter &write_buffer(u32 binding,
                                          VkDescriptorType type,
                                          VkBuffer buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize range,
                                          u32 array_element = 0);

        DescriptorSetWriter &write_uniform_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0);

        DescriptorSetWriter &write_storage_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0);

        DescriptorSetWriter &write_image(u32 binding,
                                         VkDescriptorType type,
                                         VkImageView view,
                                         VkImageLayout layout,
                                         VkSampler sampler = VK_NULL_HANDLE,
                                         u32 array_element = 0);

        DescriptorSetWriter &write_sampled_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                 u32 array_element = 0);

        DescriptorSetWriter &write_storage_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
                                                 u32 array_element = 0);

        DescriptorSetWriter &write_combined_image_sampler(u32 binding,
                                                          VkImageView view,
                                                          VkSampler sampler,
                                                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                          u32 array_element = 0);

        DescriptorSetWriter &write_sampler(u32 binding, VkSampler sampler, u32 array_element = 0);

        // A uniform/storage *texel* buffer binding, referenced through a VkBufferView (see
        // VulkanBufferView). `type` is VK_DESCRIPTOR_TYPE_{UNIFORM,STORAGE}_TEXEL_BUFFER.
        DescriptorSetWriter &write_texel_buffer(u32 binding,
                                                VkDescriptorType type,
                                                VkBufferView view,
                                                u32 array_element = 0);

        // A top-level acceleration structure binding for ray queries / ray tracing pipelines. The AS
        // handle is chained through a VkWriteDescriptorSetAccelerationStructureKHR in update().
        DescriptorSetWriter &write_acceleration_structure(u32 binding,
                                                          VkAccelerationStructureKHR acceleration_structure,
                                                          u32 array_element = 0);

        void update(VkDevice device) const;

        void clear() noexcept;

      private:
        struct BufferWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            VkDescriptorBufferInfo info{};
        };
        struct ImageWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            VkDescriptorImageInfo info{};
        };
        struct TexelWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            VkBufferView view = VK_NULL_HANDLE;
        };
        struct AccelWrite {
            u32 binding = 0;
            u32 array_element = 0;
            VkAccelerationStructureKHR acceleration_structure = VK_NULL_HANDLE;
        };

        VkDescriptorSet set_ = VK_NULL_HANDLE;
        vector<BufferWrite> buffer_writes_;
        vector<ImageWrite> image_writes_;
        vector<TexelWrite> texel_writes_;
        vector<AccelWrite> accel_writes_;
    };

} // namespace SFT::Core::Vulkan
