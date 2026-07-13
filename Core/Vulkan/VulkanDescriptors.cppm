module;
#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanDescriptors;

import :GraphicsBackendError;

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
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
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorSetLayout failed.");
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

    class DescriptorSetLayoutBuilder {
      public:
        DescriptorSetLayoutBuilder &add_binding(u32 binding,
                                                VkDescriptorType type,
                                                VkShaderStageFlags stages,
                                                u32 count = 1,
                                                const VkSampler *immutable_samplers = nullptr) {
            bindings_.push_back(VkDescriptorSetLayoutBinding{
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = count,
                .stageFlags = stages,
                .pImmutableSamplers = immutable_samplers,
            });
            binding_flags_.push_back(0);
            return *this;
        }

        DescriptorSetLayoutBuilder &set_last_binding_flags(VkDescriptorBindingFlags flags) noexcept {
            if (!binding_flags_.empty()) {
                binding_flags_.back() = flags;
            }
            return *this;
        }

        DescriptorSetLayoutBuilder &set_flags(VkDescriptorSetLayoutCreateFlags flags) noexcept {
            flags_ = flags;
            return *this;
        }

        [[nodiscard]] RendererExpected<VulkanDescriptorSetLayout> create(VkDevice device) const noexcept {
            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .pNext = nullptr,
                .bindingCount = static_cast<u32>(binding_flags_.size()),
                .pBindingFlags = binding_flags_.empty() ? nullptr : binding_flags_.data(),
            };
            bool has_binding_flags = false;
            for (VkDescriptorBindingFlags flags : binding_flags_) {
                has_binding_flags = has_binding_flags || flags != 0;
            }
            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .pNext = has_binding_flags ? &binding_flags_info : nullptr,
                .flags = flags_,
                .bindingCount = static_cast<u32>(bindings_.size()),
                .pBindings = bindings_.empty() ? nullptr : bindings_.data(),
            };
            return VulkanDescriptorSetLayout::create(device, info);
        }

        [[nodiscard]] span<const VkDescriptorSetLayoutBinding> bindings() const noexcept { return bindings_; }
        [[nodiscard]] span<const VkDescriptorBindingFlags> binding_flags() const noexcept { return binding_flags_; }

      private:
        vector<VkDescriptorSetLayoutBinding> bindings_;
        vector<VkDescriptorBindingFlags> binding_flags_;
        VkDescriptorSetLayoutCreateFlags flags_ = 0;
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
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorPool failed.");
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
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
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
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return set;
        }

        [[nodiscard]] RendererExpected<VkDescriptorSet> allocate_one(
            VkDescriptorSetLayout layout,
            u32 variable_descriptor_count) const noexcept {
            VkDescriptorSetVariableDescriptorCountAllocateInfo variable_count_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
                .pNext = nullptr,
                .descriptorSetCount = 1,
                .pDescriptorCounts = &variable_descriptor_count,
            };
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = &variable_count_info,
                .descriptorPool = pool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return set;
        }

        [[nodiscard]] RendererResult free(span<const VkDescriptorSet> sets) noexcept {
            if (vkFreeDescriptorSets(device_, pool_, static_cast<u32>(sets.size()), sets.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkFreeDescriptorSets failed.");
            return {};
        }

        // Recycles all descriptor sets allocated from this pool back to the pool.
        [[nodiscard]] RendererResult reset(VkDescriptorPoolResetFlags flags = 0) noexcept {
            if (vkResetDescriptorPool(device_, pool_, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetDescriptorPool failed.");
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

    class VulkanDescriptorSet {
      public:
        VulkanDescriptorSet() = default;
        explicit VulkanDescriptorSet(VkDescriptorSet set) noexcept : set_(set) {}

        [[nodiscard]] VkDescriptorSet vk_handle() const noexcept { return set_; }
        [[nodiscard]] bool is_valid() const noexcept { return set_ != VK_NULL_HANDLE; }
        [[nodiscard]] explicit operator bool() const noexcept { return is_valid(); }

      private:
        VkDescriptorSet set_ = VK_NULL_HANDLE;
    };

    class DescriptorSetWriter {
      public:
        DescriptorSetWriter &set_descriptor_set(VkDescriptorSet set) noexcept {
            set_ = set;
            return *this;
        }

        DescriptorSetWriter &write_buffer(u32 binding,
                                          VkDescriptorType type,
                                          VkBuffer buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize range,
                                          u32 array_element = 0) {
            buffer_writes_.push_back(BufferWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .info = VkDescriptorBufferInfo{
                    .buffer = buffer,
                    .offset = offset,
                    .range = range,
                },
            });
            return *this;
        }

        DescriptorSetWriter &write_uniform_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0) {
            return write_buffer(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, buffer, offset, range, array_element);
        }

        DescriptorSetWriter &write_storage_buffer(u32 binding,
                                                  VkBuffer buffer,
                                                  VkDeviceSize offset,
                                                  VkDeviceSize range,
                                                  u32 array_element = 0) {
            return write_buffer(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer, offset, range, array_element);
        }

        DescriptorSetWriter &write_image(u32 binding,
                                         VkDescriptorType type,
                                         VkImageView view,
                                         VkImageLayout layout,
                                         VkSampler sampler = VK_NULL_HANDLE,
                                         u32 array_element = 0) {
            image_writes_.push_back(ImageWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .info = VkDescriptorImageInfo{
                    .sampler = sampler,
                    .imageView = view,
                    .imageLayout = layout,
                },
            });
            return *this;
        }

        DescriptorSetWriter &write_sampled_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                 u32 array_element = 0) {
            return write_image(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout, VK_NULL_HANDLE, array_element);
        }

        DescriptorSetWriter &write_storage_image(u32 binding,
                                                 VkImageView view,
                                                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL,
                                                 u32 array_element = 0) {
            return write_image(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, layout, VK_NULL_HANDLE, array_element);
        }

        DescriptorSetWriter &write_combined_image_sampler(u32 binding,
                                                          VkImageView view,
                                                          VkSampler sampler,
                                                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                          u32 array_element = 0) {
            return write_image(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, view, layout, sampler, array_element);
        }

        DescriptorSetWriter &write_sampler(u32 binding, VkSampler sampler, u32 array_element = 0) {
            return write_image(binding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, sampler, array_element);
        }

        // A uniform/storage *texel* buffer binding, referenced through a VkBufferView (see
        // VulkanBufferView). `type` is VK_DESCRIPTOR_TYPE_{UNIFORM,STORAGE}_TEXEL_BUFFER.
        DescriptorSetWriter &write_texel_buffer(u32 binding,
                                                VkDescriptorType type,
                                                VkBufferView view,
                                                u32 array_element = 0) {
            texel_writes_.push_back(TexelWrite{
                .binding = binding,
                .array_element = array_element,
                .type = type,
                .view = view,
            });
            return *this;
        }

        // A top-level acceleration structure binding for ray queries / ray tracing pipelines. The AS
        // handle is chained through a VkWriteDescriptorSetAccelerationStructureKHR in update().
        DescriptorSetWriter &write_acceleration_structure(u32 binding,
                                                          VkAccelerationStructureKHR acceleration_structure,
                                                          u32 array_element = 0) {
            accel_writes_.push_back(AccelWrite{
                .binding = binding,
                .array_element = array_element,
                .acceleration_structure = acceleration_structure,
            });
            return *this;
        }

        void update(VkDevice device) const {
            vector<VkWriteDescriptorSet> writes;
            writes.reserve(buffer_writes_.size() + image_writes_.size() + texel_writes_.size() +
                           accel_writes_.size());
            // Stable backing storage for the acceleration-structure pNext structs — pointers into this
            // vector must stay valid until vkUpdateDescriptorSets returns, so it lives out here.
            vector<VkWriteDescriptorSetAccelerationStructureKHR> accel_infos;
            accel_infos.reserve(accel_writes_.size());

            for (const BufferWrite &write : buffer_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = nullptr,
                    .pBufferInfo = &write.info,
                    .pTexelBufferView = nullptr,
                });
            }
            for (const ImageWrite &write : image_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = &write.info,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                });
            }

            for (const TexelWrite &write : texel_writes_) {
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = nullptr,
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = write.type,
                    .pImageInfo = nullptr,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = &write.view,
                });
            }
            for (const AccelWrite &write : accel_writes_) {
                accel_infos.push_back(VkWriteDescriptorSetAccelerationStructureKHR{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                    .pNext = nullptr,
                    .accelerationStructureCount = 1,
                    .pAccelerationStructures = &write.acceleration_structure,
                });
                writes.push_back(VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = &accel_infos.back(),
                    .dstSet = set_,
                    .dstBinding = write.binding,
                    .dstArrayElement = write.array_element,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                    .pImageInfo = nullptr,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                });
            }

            vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.empty() ? nullptr : writes.data(), 0, nullptr);
        }

        void clear() noexcept {
            buffer_writes_.clear();
            image_writes_.clear();
            texel_writes_.clear();
            accel_writes_.clear();
        }

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
