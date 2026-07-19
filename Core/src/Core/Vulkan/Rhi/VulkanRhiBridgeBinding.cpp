// RhiDevice binding-model resources: bind group layouts, bind groups (ordinary ones share a growable
// pool of DescriptorPoolChunks; update-after-bind/variable-count/acceleration-structure ones keep a
// dedicated exactly-sized pool — see VulkanRhiBridge.hpp's BindGroupRecord comment), and pipeline
// layouts. Mirrors RHI's own :Binding partition, which groups these three together.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <array>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanDescriptors.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPipeline.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

using std::array;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        // Sized generously relative to how many descriptors one ordinary bind group actually uses
        // (typically 1-3): kMaxSetsPerChunk sets rarely exhaust any one type's budget first, so in
        // practice a chunk's lifetime is bounded by set count, not descriptor count. A frame's whole
        // per-pass bind-group churn (a double-digit count, not hundreds) fits many times over in one
        // chunk, so steady state costs zero pool creation at all after the first one or two chunks.
        constexpr u32 kDescriptorPoolChunkMaxSets = 256;
        constexpr u32 kDescriptorPoolChunkTypeCapacity = 1024;

        [[nodiscard]] RendererExpected<VulkanDescriptorPool> create_descriptor_pool_chunk(VkDevice device) {
            // A cold-start/growth event, not a per-frame one — if this ever logs more than a handful
            // of times over an application's lifetime, something is either creating far more bind
            // groups per frame than this renderer's built-in passes do, or churning through them fast
            // enough to matter; worth knowing either way, the same reasoning as this file's other
            // one-time-cost logs (shader compiles, pipeline builds).
            Foundation::log_info("Vulkan: creating a new shared descriptor pool chunk ({} sets, {} descriptors/type capacity).",
                                 kDescriptorPoolChunkMaxSets, kDescriptorPoolChunkTypeCapacity);
            const array<VkDescriptorPoolSize, 7> sizes{
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
                VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = kDescriptorPoolChunkTypeCapacity},
            };
            return VulkanDescriptorPool::create_from_sizes(device, sizes, kDescriptorPoolChunkMaxSets,
                                                            VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
        }

    } // namespace

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

        // Mirror the layout's bindless requirements: an update-after-bind layout can only be allocated
        // from an update-after-bind pool, and a variable-count final binding's actual size is chosen
        // here (we use the layout's declared count as the size). Acceleration-structure descriptors
        // also skip the shared-chunk path below — rare/bindless-shaped in practice, and giving them
        // their own exactly-sized pool avoids every shared chunk needing AS descriptor budget it will
        // almost never use.
        bool needs_update_after_bind = false;
        bool has_variable_count = false;
        bool has_acceleration_structure = false;
        u32 variable_count = 0;
        for (const rhi::BindGroupLayoutEntry &entry : layout_record->entries) {
            if (rhi::has_any(entry.flags, rhi::BindingFlags::UpdateAfterBind)) {
                needs_update_after_bind = true;
            }
            if (rhi::has_any(entry.flags, rhi::BindingFlags::VariableDescriptorCount)) {
                has_variable_count = true;
                variable_count = entry.count;
            }
            if (entry.type == rhi::BindingType::AccelerationStructure) {
                has_acceleration_structure = true;
            }
        }

        VulkanDescriptorPool dedicated_pool;
        VkDescriptorSet set = VK_NULL_HANDLE;
        i32 shared_chunk_index = -1;

        if (needs_update_after_bind || has_variable_count || has_acceleration_structure) {
            // Dedicated pool sized to exactly this one set — unchanged from how this engine has always
            // handled these rarer, bindless-shaped cases (see BindGroupRecord's doc comment).
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
            const VkDescriptorPoolCreateFlags pool_flags =
                needs_update_after_bind
                    ? static_cast<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)
                    : 0u;

            auto pool = VulkanDescriptorPool::create_from_sizes(logical_device_->vk_handle(), pool_sizes, 1, pool_flags);
            if (!pool) {
                return rhi_error_from_graphics(pool.error());
            }
            auto allocated = has_variable_count
                           ? pool->allocate_one(layout_record->layout.vk_handle(), variable_count)
                           : pool->allocate_one(layout_record->layout.vk_handle());
            if (!allocated) {
                return rhi_error_from_graphics(allocated.error());
            }
            dedicated_pool = std::move(*pool);
            set = *allocated;
        } else {
            // Common case: allocate from the current shared chunk, growing (appending and switching to
            // a fresh chunk) whenever the current one can't satisfy this request. This is the path that
            // replaces one-vkCreateDescriptorPool-per-bind-group with amortized-to-near-zero steady-
            // state pool creation — see create_descriptor_pool_chunk()'s doc comment above.
            auto chunks = descriptor_pool_chunks_.lock();
            if (chunks->empty()) {
                auto pool = create_descriptor_pool_chunk(logical_device_->vk_handle());
                if (!pool) {
                    return rhi_error_from_graphics(pool.error());
                }
                chunks->push_back(DescriptorPoolChunk{.pool = std::move(*pool)});
            }
            auto allocated = chunks->back().pool.allocate_one(layout_record->layout.vk_handle());
            if (!allocated) {
                auto pool = create_descriptor_pool_chunk(logical_device_->vk_handle());
                if (!pool) {
                    return rhi_error_from_graphics(pool.error());
                }
                chunks->push_back(DescriptorPoolChunk{.pool = std::move(*pool)});
                allocated = chunks->back().pool.allocate_one(layout_record->layout.vk_handle());
                if (!allocated) {
                    return rhi_error_from_graphics(allocated.error());
                }
            }
            shared_chunk_index = static_cast<i32>(chunks->size() - 1);
            ++(*chunks)[static_cast<usize>(shared_chunk_index)].live_sets;
            set = *allocated;
        }

        // Building the descriptor writes can still fail on a malformed desc (an entry referencing a
        // binding/handle that doesn't exist) even though the set itself was allocated fine. Collecting
        // that failure here instead of returning directly lets the shared-chunk path below free the
        // set back to its chunk on the way out — the dedicated-pool path doesn't need that (the local
        // `dedicated_pool` simply gets destroyed once this function returns, taking the set with it).
        const rhi::RhiExpected<void> write_result = [&]() -> rhi::RhiExpected<void> {
        DescriptorSetWriter writer;
        writer.set_descriptor_set(set);
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
        return {};
        }();

        if (!write_result.has_value()) {
            if (shared_chunk_index >= 0) {
                auto chunks = descriptor_pool_chunks_.lock();
                DescriptorPoolChunk &chunk = (*chunks)[static_cast<usize>(shared_chunk_index)];
                (void)chunk.pool.free(span<const VkDescriptorSet>{&set, 1});
                --chunk.live_sets;
            }
            return std::unexpected(write_result.error());
        }

        return bind_groups_.insert(BindGroupRecord{std::move(dedicated_pool), set, shared_chunk_index});
    }

    void VulkanRhiDeviceBridge::destroy_bind_group(rhi::BindGroupHandle handle) noexcept {
        BindGroupRecord *record = bind_groups_.find(handle);
        if (record == nullptr) {
            return;
        }
        if (record->shared_chunk_index >= 0) {
            auto chunks = descriptor_pool_chunks_.lock();
            DescriptorPoolChunk &chunk = (*chunks)[static_cast<usize>(record->shared_chunk_index)];
            (void)chunk.pool.free(span<const VkDescriptorSet>{&record->set, 1});
            --chunk.live_sets;
        }
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
