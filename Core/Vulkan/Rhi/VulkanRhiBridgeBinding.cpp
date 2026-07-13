// RhiDevice binding-model resources: bind group layouts, bind groups (each backed by its own
// exactly-sized descriptor pool — see VulkanRhiBridge.cppm's BindGroupRecord comment), and pipeline
// layouts. Mirrors RHI's own :Binding partition, which groups these three together.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <utility>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanDescriptors;
import :VulkanDevice;
import :VulkanPipeline;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import Sturdy.Foundation;
import Sturdy.RHI;

using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::BindGroupLayoutHandle> VulkanRhiDeviceBridge::create_bind_group_layout(
        const rhi::BindGroupLayoutDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::BindGroupLayoutHandle>("create_bind_group_layout");
        }

        vector<VkDescriptorSetLayoutBinding> bindings;
        vector<VkDescriptorBindingFlags> binding_flags;
        bindings.reserve(desc.entries.size());
        binding_flags.reserve(desc.entries.size());
        bool any_binding_flags = false;
        bool any_update_after_bind = false;
        for (const rhi::BindGroupLayoutEntry &entry : desc.entries) {
            bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = entry.binding,
                .descriptorType = to_vk(entry.type),
                .descriptorCount = entry.count,
                .stageFlags = to_vk(entry.visibility),
                .pImmutableSamplers = nullptr,
            });
            const VkDescriptorBindingFlags vk_flags = to_vk(entry.flags);
            binding_flags.push_back(vk_flags);
            any_binding_flags = any_binding_flags || vk_flags != 0;
            any_update_after_bind = any_update_after_bind ||
                                    rhi::has_any(entry.flags, rhi::BindingFlags::UpdateAfterBind);
        }

        // A layout with any bindless descriptor-indexing flag needs the flags chained through pNext;
        // update-after-bind additionally requires the matching create-flag (and an update-after-bind
        // pool at bind-group time — see create_bind_group below).
        const VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext = nullptr,
            .bindingCount = static_cast<u32>(binding_flags.size()),
            .pBindingFlags = binding_flags.empty() ? nullptr : binding_flags.data(),
        };
        const VkDescriptorSetLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = any_binding_flags ? &binding_flags_info : nullptr,
            .flags = any_update_after_bind
                         ? static_cast<VkDescriptorSetLayoutCreateFlags>(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)
                         : 0u,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.empty() ? nullptr : bindings.data(),
        };
        auto layout = VulkanDescriptorSetLayout::create(logical_device_->vk_handle(), layout_info);
        if (!layout) {
            return rhi_error_from_graphics(layout.error());
        }

        return bind_group_layouts_.insert(BindGroupLayoutRecord{
            std::move(*layout),
            vector<rhi::BindGroupLayoutEntry>(desc.entries.begin(), desc.entries.end()),
        });
    }

    void VulkanRhiDeviceBridge::destroy_bind_group_layout(rhi::BindGroupLayoutHandle handle) noexcept {
        bind_group_layouts_.erase(handle);
    }

    rhi::RhiExpected<rhi::BindGroupHandle> VulkanRhiDeviceBridge::create_bind_group(const rhi::BindGroupDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::BindGroupHandle>("create_bind_group");
        }

        BindGroupLayoutRecord *layout_record = bind_group_layouts_.find(desc.layout);
        if (layout_record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_bind_group: unknown bind group layout handle.");
        }

        // Size a pool for exactly this one set: aggregate the layout's entries by descriptor type.
        vector<VkDescriptorPoolSize> pool_sizes;
        for (const rhi::BindGroupLayoutEntry &entry : layout_record->entries) {
            const VkDescriptorType type = to_vk(entry.type);
            bool merged = false;
            for (VkDescriptorPoolSize &size : pool_sizes) {
                if (size.type == type) {
                    size.descriptorCount += entry.count;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                pool_sizes.push_back(VkDescriptorPoolSize{.type = type, .descriptorCount = entry.count});
            }
        }
        if (pool_sizes.empty()) {
            pool_sizes.push_back(VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1});
        }

        // Mirror the layout's bindless requirements onto its pool/allocation: an update-after-bind
        // layout can only be allocated from an update-after-bind pool, and a variable-count final
        // binding's actual size is chosen here (we use the layout's declared count as the size).
        bool needs_update_after_bind = false;
        bool has_variable_count = false;
        u32 variable_count = 0;
        for (const rhi::BindGroupLayoutEntry &entry : layout_record->entries) {
            if (rhi::has_any(entry.flags, rhi::BindingFlags::UpdateAfterBind)) {
                needs_update_after_bind = true;
            }
            if (rhi::has_any(entry.flags, rhi::BindingFlags::VariableDescriptorCount)) {
                has_variable_count = true;
                variable_count = entry.count;
            }
        }
        const VkDescriptorPoolCreateFlags pool_flags =
            needs_update_after_bind
                ? static_cast<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)
                : 0u;

        auto pool = VulkanDescriptorPool::create_from_sizes(logical_device_->vk_handle(), pool_sizes, 1, pool_flags);
        if (!pool) {
            return rhi_error_from_graphics(pool.error());
        }

        auto set = has_variable_count
                       ? pool->allocate_one(layout_record->layout.vk_handle(), variable_count)
                       : pool->allocate_one(layout_record->layout.vk_handle());
        if (!set) {
            return rhi_error_from_graphics(set.error());
        }

        DescriptorSetWriter writer;
        writer.set_descriptor_set(*set);
        for (const rhi::BindGroupEntry &entry : desc.entries) {
            const rhi::BindGroupLayoutEntry *layout_entry = nullptr;
            for (const rhi::BindGroupLayoutEntry &candidate : layout_record->entries) {
                if (candidate.binding == entry.binding) {
                    layout_entry = &candidate;
                    break;
                }
            }
            if (layout_entry == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_bind_group: entry binding is not present in the bind group's layout.");
            }

            switch (layout_entry->type) {
                case rhi::BindingType::UniformBuffer:
                case rhi::BindingType::StorageBuffer:
                case rhi::BindingType::ReadOnlyStorageBuffer: {
                    BufferRecord *buffer_record = buffers_.find(entry.buffer);
                    if (buffer_record == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                              "create_bind_group: unknown buffer handle for a buffer binding.");
                    }
                    const VkDeviceSize range = entry.size == 0 ? VK_WHOLE_SIZE : static_cast<VkDeviceSize>(entry.size);
                    writer.write_buffer(entry.binding, to_vk(layout_entry->type), buffer_record->buffer.vk_handle(),
                                        entry.offset, range);
                    break;
                }
                case rhi::BindingType::SampledTexture:
                case rhi::BindingType::StorageTexture:
                case rhi::BindingType::InputAttachment: {
                    VulkanImageView *view = texture_views_.find(entry.texture_view);
                    if (view == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                              "create_bind_group: unknown texture view handle for an image binding.");
                    }
                    const VkImageLayout layout_for_image = layout_entry->type == rhi::BindingType::StorageTexture
                                                                ? VK_IMAGE_LAYOUT_GENERAL
                                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    writer.write_image(entry.binding, to_vk(layout_entry->type), view->vk_handle(), layout_for_image);
                    break;
                }
                case rhi::BindingType::Sampler: {
                    VulkanSampler *sampler = samplers_.find(entry.sampler);
                    if (sampler == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                              "create_bind_group: unknown sampler handle for a sampler binding.");
                    }
                    writer.write_sampler(entry.binding, sampler->vk_handle());
                    break;
                }
                case rhi::BindingType::CombinedImageSampler: {
                    VulkanImageView *view = texture_views_.find(entry.texture_view);
                    VulkanSampler *sampler = samplers_.find(entry.sampler);
                    if (view == nullptr || sampler == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                              "create_bind_group: unknown texture view/sampler handle for a "
                                              "combined-image-sampler binding.");
                    }
                    writer.write_combined_image_sampler(entry.binding, view->vk_handle(), sampler->vk_handle());
                    break;
                }
                case rhi::BindingType::AccelerationStructure: {
                    AccelerationStructureRecord *as_record = acceleration_structures_.find(entry.acceleration_structure);
                    if (as_record == nullptr) {
                        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                              "create_bind_group: unknown acceleration structure handle for an AS binding.");
                    }
                    writer.write_acceleration_structure(entry.binding, as_record->acceleration_structure.vk_handle());
                    break;
                }
            }
        }
        writer.update(logical_device_->vk_handle());

        return bind_groups_.insert(BindGroupRecord{std::move(*pool), *set});
    }

    void VulkanRhiDeviceBridge::destroy_bind_group(rhi::BindGroupHandle handle) noexcept {
        bind_groups_.erase(handle);
    }

    rhi::RhiExpected<rhi::PipelineLayoutHandle> VulkanRhiDeviceBridge::create_pipeline_layout(
        const rhi::PipelineLayoutDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::PipelineLayoutHandle>("create_pipeline_layout");
        }

        vector<VkDescriptorSetLayout> set_layouts;
        set_layouts.reserve(desc.bind_group_layouts.size());
        for (rhi::BindGroupLayoutHandle handle : desc.bind_group_layouts) {
            BindGroupLayoutRecord *record = bind_group_layouts_.find(handle);
            if (record == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_pipeline_layout: unknown bind group layout handle.");
            }
            set_layouts.push_back(record->layout.vk_handle());
        }

        vector<VkPushConstantRange> push_constants;
        push_constants.reserve(desc.push_constant_ranges.size());
        for (const rhi::PushConstantRange &range : desc.push_constant_ranges) {
            push_constants.push_back(VkPushConstantRange{
                .stageFlags = to_vk(range.stages),
                .offset = range.offset,
                .size = range.size,
            });
        }

        auto layout = VulkanPipelineLayout::create_from_sets(logical_device_->vk_handle(), set_layouts, push_constants);
        if (!layout) {
            return rhi_error_from_graphics(layout.error());
        }
        return pipeline_layouts_.insert(std::move(*layout));
    }

    void VulkanRhiDeviceBridge::destroy_pipeline_layout(rhi::PipelineLayoutHandle handle) noexcept {
        pipeline_layouts_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
