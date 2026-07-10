// RhiDevice ray tracing pipeline creation/destruction + shader-group-handle readback.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <expected>
#include <span>
#include <utility>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanPipeline;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import Sturdy.Foundation;
import Sturdy.RHI;

using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        [[nodiscard]] constexpr VkRayTracingShaderGroupTypeKHR shader_group_type_to_vk(rhi::RayTracingShaderGroupType type) noexcept {
            switch (type) {
                case rhi::RayTracingShaderGroupType::General: return VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                case rhi::RayTracingShaderGroupType::TrianglesHitGroup: return VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
                case rhi::RayTracingShaderGroupType::ProceduralHitGroup: return VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
            }
            return VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        }

        [[nodiscard]] bool is_single_stage(rhi::ShaderStage stage) noexcept {
            const VkShaderStageFlags flags = SFT::Core::Vulkan::to_vk(stage);
            return flags != 0 && (flags & (flags - 1)) == 0;
        }

        [[nodiscard]] bool is_valid_general_stage(rhi::ShaderStage stage) noexcept {
            return stage == rhi::ShaderStage::RayGeneration || stage == rhi::ShaderStage::Miss ||
                   stage == rhi::ShaderStage::Callable;
        }


    } // namespace

    rhi::RhiExpected<rhi::RayTracingPipelineHandle> VulkanRhiDeviceBridge::create_ray_tracing_pipeline(
        const rhi::RayTracingPipelineDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::RayTracingPipelineHandle>("create_ray_tracing_pipeline");
        }
        if (!enabled_features_.has(rhi::Feature::RayTracingPipeline)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_ray_tracing_pipeline: requires enabled Feature::RayTracingPipeline.");
        }
        if (vkCreateRayTracingPipelinesKHR == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_ray_tracing_pipeline: ray-tracing pipeline entry point is not loaded.");
        }

        VulkanPipelineLayout *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_ray_tracing_pipeline: unknown pipeline layout handle.");
        }

        vector<VkPipelineShaderStageCreateInfo> stages;
        vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        stages.reserve(desc.groups.size() * 3);
        groups.reserve(desc.groups.size());

        auto add_stage = [&](const rhi::ShaderEntry &entry,
                             rhi::ShaderStage implied_stage) -> rhi::RhiExpected<u32> {
            if (!entry.module.is_valid()) {
                return VK_SHADER_UNUSED_KHR;
            }

            const rhi::ShaderStage stage = entry.stage == rhi::ShaderStage::None ? implied_stage : entry.stage;
            if (!is_single_stage(stage)) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_ray_tracing_pipeline: shader entry must name exactly one stage.");
            }
            if (implied_stage != rhi::ShaderStage::None && stage != implied_stage) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_ray_tracing_pipeline: shader entry stage does not match its group field.");
            }

            VkShaderModule *module = shader_modules_.find(entry.module);
            if (module == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_ray_tracing_pipeline: unknown shader module handle.");
            }

            const u32 index = static_cast<u32>(stages.size());
            stages.push_back(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = to_vk_single_stage(stage),
                .module = *module,
                .pName = entry.entry_point,
            });
            return index;
        };

        for (const rhi::RayTracingShaderGroupDesc &group : desc.groups) {
            VkRayTracingShaderGroupCreateInfoKHR vk_group{
                .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .type = shader_group_type_to_vk(group.type),
                .generalShader = VK_SHADER_UNUSED_KHR,
                .closestHitShader = VK_SHADER_UNUSED_KHR,
                .anyHitShader = VK_SHADER_UNUSED_KHR,
                .intersectionShader = VK_SHADER_UNUSED_KHR,
            };

            if (group.type == rhi::RayTracingShaderGroupType::General) {
                if (!group.general.module.is_valid()) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                          "create_ray_tracing_pipeline: general shader group requires a general shader.");
                }
                if (!is_valid_general_stage(group.general.stage)) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                          "create_ray_tracing_pipeline: general shader stage must be RayGeneration, Miss, or Callable.");
                }
                auto stage = add_stage(group.general, rhi::ShaderStage::None);
                if (!stage) {
                    return std::unexpected(stage.error());
                }
                vk_group.generalShader = *stage;
            } else {
                auto closest = add_stage(group.closest_hit, rhi::ShaderStage::ClosestHit);
                if (!closest) return std::unexpected(closest.error());
                auto any = add_stage(group.any_hit, rhi::ShaderStage::AnyHit);
                if (!any) return std::unexpected(any.error());
                auto intersection = add_stage(group.intersection, rhi::ShaderStage::Intersection);
                if (!intersection) return std::unexpected(intersection.error());

                vk_group.closestHitShader = *closest;
                vk_group.anyHitShader = *any;
                vk_group.intersectionShader = *intersection;
            }

            groups.push_back(vk_group);
        }

        const VkRayTracingPipelineCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .stageCount = static_cast<u32>(stages.size()),
            .pStages = stages.empty() ? nullptr : stages.data(),
            .groupCount = static_cast<u32>(groups.size()),
            .pGroups = groups.empty() ? nullptr : groups.data(),
            .maxPipelineRayRecursionDepth = desc.max_ray_recursion_depth,
            .layout = layout->vk_handle(),
        };
        auto pipeline = VulkanPipeline::create_ray_tracing(logical_device_->vk_handle(), VK_NULL_HANDLE, info);
        if (!pipeline) {
            return rhi_error_from_graphics(pipeline.error());
        }
        return ray_tracing_pipelines_.insert(PipelineRecord{std::move(*pipeline), desc.layout});
    }

    void VulkanRhiDeviceBridge::destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept {
        ray_tracing_pipelines_.erase(handle);
    }

    rhi::RhiResult VulkanRhiDeviceBridge::write_ray_tracing_shader_group_handles(
        rhi::RayTracingPipelineHandle pipeline, u32 first_group, u32 group_count, span<std::byte> dst) {
        if (!enabled_features_.has(rhi::Feature::RayTracingPipeline)) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "write_ray_tracing_shader_group_handles: requires enabled Feature::RayTracingPipeline.");
        }
        PipelineRecord *record = ray_tracing_pipelines_.find(pipeline);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "write_ray_tracing_shader_group_handles: unknown ray tracing pipeline handle.");
        }

        span<u8> bytes(reinterpret_cast<u8 *>(dst.data()), dst.size());
        if (auto result = record->pipeline.get_ray_tracing_shader_group_handles(first_group, group_count, bytes); !result) {
            return rhi_error_from_graphics(result.error());
        }
        return {};
    }

} // namespace SFT::Core::Vulkan
