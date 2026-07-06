// VulkanBackend graphics pipeline construction: the fixed-function state block and the
// dynamic-rendering pipeline built from the compiled triangle shader modules.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <format>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanBackend;
import :VulkanConstants;
import :VulkanDevice;
import :VulkanPipeline;
import :VulkanShaderModule;
import :RendererError;
import :Renderer;
import Sturdy.Foundation;

using std::format;
using std::vector;

namespace SFT::Core::Vulkan {

    RendererResult VulkanBackend::createGraphicsPipeline(const RendererCreateInfo &init) {
        (void)init;
        //define a pipeline layout
        auto pipeline_layout_result = VulkanPipelineLayout::create_empty(this->logicalDevice.vk_handle());
        if (!pipeline_layout_result.has_value()) [[unlikely]] {
            return renderer_error(pipeline_layout_result.error().code,
                                  format("Failed to create pipeline layout: {}", pipeline_layout_result.error().message));
        }
        this->pipelinelayout = std::move(*pipeline_layout_result);

        auto *vert = find_shader_module("Shaders/triangle", "vertexMain");
        auto *frag = find_shader_module("Shaders/triangle", "fragmentMain");
        if (!vert or !frag) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  "Vulkan graphics pipeline requires 'Shaders/triangle' vertexMain/fragmentMain shader modules.");
        }

        // stage_info() builds the create info from the module's own stage + entry point (stored as
        // an owned UString, so .pName stays valid), rather than reaching into a borrowed ustr here.
        const vector<VkPipelineShaderStageCreateInfo> shaderStages{
            vert->stage_info(),
            frag->stage_info(),
        };

        //Vertex Pulling, don't define
        VkPipelineVertexInputStateCreateInfo vertInputInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };

        //input assembly, we'll be drawing triangle lists
        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        };

        //depth/stencil config
        VkPipelineDepthStencilStateCreateInfo stencilInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .stencilTestEnable = VK_FALSE
        };

        // Dynamic viewport/scissor state means these values are overwritten at draw time, but keep
        // valid backing pointers here so strict drivers never see nonzero counts with null arrays.
        VkViewport viewport{};
        VkRect2D scissor{};
        VkPipelineViewportStateCreateInfo viewportInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };

        //raster settings
        // frontFace is CLOCKWISE, not the GL-conventional CCW: our viewport (set_viewport() above)
        // uses a positive height with no Y-flip, so Vulkan's y-down NDC makes vertices authored in
        // the usual (x right, y up) math sense wind clockwise once rasterized in screen space.
        VkPipelineRasterizationStateCreateInfo rasterizationInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };

        //multisampling settings
        VkPipelineMultisampleStateCreateInfo multisampleInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };

        // Alpha-blending
        VkPipelineColorBlendAttachmentState attachState
        {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };
        VkPipelineColorBlendStateCreateInfo blendInfo
        {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &attachState
        };

        //begin dynamic rendering
        vector<VkDynamicState> dynamicState
        {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicStateInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<u32>(dynamicState.size()),
            .pDynamicStates = dynamicState.data()
        };

        //structure required for dynamic rendering
        VkPipelineRenderingCreateInfo renderInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &SWAPCHAIN_FORMAT,
            .depthAttachmentFormat = DEPTH_FORMAT
        };

        //create the graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo
        {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderInfo,
            .stageCount = static_cast<u32>(shaderStages.size()),
            .pStages = shaderStages.data(),
            .pVertexInputState = &vertInputInfo,
            .pInputAssemblyState = &inputAssemblyInfo,
            .pViewportState = &viewportInfo,
            .pRasterizationState = &rasterizationInfo,
            .pMultisampleState = &multisampleInfo,
            .pDepthStencilState = &stencilInfo,
            .pColorBlendState = &blendInfo,
            .pDynamicState = &dynamicStateInfo,
            .layout = pipelinelayout.vk_handle(),
            .renderPass = VK_NULL_HANDLE
        };

        auto pipeline_result = VulkanPipeline::create_graphics_dynamic(this->logicalDevice.vk_handle(), VK_NULL_HANDLE, pipelineInfo);
        if (!pipeline_result.has_value()) [[unlikely]] {
            return renderer_error(pipeline_result.error().code,
                                  format("Failed to create Vulkan graphics pipeline: {}", pipeline_result.error().message));
        }

        this->graphicsPipeline = std::move(*pipeline_result);

        Foundation::log_info("Vulkan graphics pipeline created.");
        return {};
    }

} // namespace SFT::Core::Vulkan
