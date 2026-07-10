module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanPipeline;

import :GraphicsBackendError;
import Sturdy.Foundation;

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::span;
using std::vector;

export namespace SFT::Core::Vulkan {

    // ─── VulkanPipelineLayout ────────────────────────────────────────────────────

    class VulkanPipelineLayout {
      public:
        VulkanPipelineLayout() = default;
        ~VulkanPipelineLayout() { destroy(); }

        VulkanPipelineLayout(const VulkanPipelineLayout &) = delete;
        VulkanPipelineLayout &operator=(const VulkanPipelineLayout &) = delete;

        VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }
        VulkanPipelineLayout &operator=(VulkanPipelineLayout &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create(
            VkDevice device,
            const VkPipelineLayoutCreateInfo &info) noexcept {
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreatePipelineLayout failed.");
            VulkanPipelineLayout out;
            out.device_ = device;
            out.layout_ = layout;
            return out;
        }

        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_from_sets(
            VkDevice device,
            span<const VkDescriptorSetLayout> set_layouts,
            span<const VkPushConstantRange> push_constants = {}) noexcept {
            VkPipelineLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = static_cast<u32>(set_layouts.size()),
                .pSetLayouts = set_layouts.empty() ? nullptr : set_layouts.data(),
                .pushConstantRangeCount = static_cast<u32>(push_constants.size()),
                .pPushConstantRanges = push_constants.empty() ? nullptr : push_constants.data(),
            };
            return create(device, info);
        }

        // Convenience: empty layout (no push constants, no descriptor sets).
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_empty(VkDevice device) noexcept {
            VkPipelineLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 0,
                .pSetLayouts = nullptr,
                .pushConstantRangeCount = 0,
                .pPushConstantRanges = nullptr,
            };
            return create(device, info);
        }

        [[nodiscard]] VkPipelineLayout vk_handle() const noexcept { return layout_; }
        [[nodiscard]] bool is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

        void destroy() noexcept {
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
    };

    class PipelineLayoutBuilder {
      public:
        PipelineLayoutBuilder &add_set_layout(VkDescriptorSetLayout layout) {
            set_layouts_.push_back(layout);
            return *this;
        }
        PipelineLayoutBuilder &set_set_layouts(span<const VkDescriptorSetLayout> layouts) {
            set_layouts_.assign(layouts.begin(), layouts.end());
            return *this;
        }
        PipelineLayoutBuilder &add_push_constant_range(VkShaderStageFlags stages, u32 offset, u32 size) {
            push_constants_.push_back(VkPushConstantRange{
                .stageFlags = stages,
                .offset = offset,
                .size = size,
            });
            return *this;
        }
        [[nodiscard]] RendererExpected<VulkanPipelineLayout> create(VkDevice device) const noexcept {
            return VulkanPipelineLayout::create_from_sets(device, set_layouts_, push_constants_);
        }

      private:
        vector<VkDescriptorSetLayout> set_layouts_;
        vector<VkPushConstantRange> push_constants_;
    };

    struct VulkanGraphicsPipelineSignature {
        vector<VkFormat> color_formats;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        u32 view_mask = 0;

        [[nodiscard]] VkPipelineRenderingCreateInfo rendering_info() const noexcept {
            return VkPipelineRenderingCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = view_mask,
                .colorAttachmentCount = static_cast<u32>(color_formats.size()),
                .pColorAttachmentFormats = color_formats.empty() ? nullptr : color_formats.data(),
                .depthAttachmentFormat = depth_format,
                .stencilAttachmentFormat = stencil_format,
            };
        }
    };

    // ─── VulkanPipeline ──────────────────────────────────────────────────────────

    class VulkanPipeline {
      public:
        VulkanPipeline() = default;
        ~VulkanPipeline() { destroy(); }

        VulkanPipeline(const VulkanPipeline &) = delete;
        VulkanPipeline &operator=(const VulkanPipeline &) = delete;

        VulkanPipeline(VulkanPipeline &&o) noexcept
            : device_(o.device_), pipeline_(o.pipeline_), bind_point_(o.bind_point_) {
            o.device_ = VK_NULL_HANDLE;
            o.pipeline_ = VK_NULL_HANDLE;
        }
        VulkanPipeline &operator=(VulkanPipeline &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pipeline_ = o.pipeline_;
                bind_point_ = o.bind_point_;
                o.device_ = VK_NULL_HANDLE;
                o.pipeline_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // For pipelines used with traditional render passes.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics(
            VkDevice device,
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            if (device == VK_NULL_HANDLE)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines called with a null VkDevice.");
            if (vkCreateGraphicsPipelines == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines is not loaded. Call volkLoadDevice after device creation.");

            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

        // For pipelines used with vkCmdBeginRendering (Vulkan 1.3+ dynamic rendering).
        // Set info.renderPass = VK_NULL_HANDLE and chain a VkPipelineRenderingCreateInfo
        // (from PipelineRenderingInfo::to_vk() or VulkanGraphicsPipelineSignature::rendering_info())
        // into info.pNext describing the attachment formats.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics_dynamic(
            VkDevice device,
            VkPipelineCache cache,
            VkGraphicsPipelineCreateInfo info // taken by value so we can assert renderPass is null
            ) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkCreateGraphicsPipelines (dynamic rendering) failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

        [[nodiscard]] static RendererExpected<VulkanPipeline> create_compute(
            VkDevice device,
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateComputePipelines failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_COMPUTE;
            return out;
        }

        // Ray tracing pipeline (RayTracingPipeline feature). `deferred_op` may be VK_NULL_HANDLE for a
        // blocking build, or a VkDeferredOperationKHR to offload compilation onto worker threads
        // (DeferredHostOperations). Function-pointer-guarded — the entry point is null unless the
        // ray-tracing-pipeline extension was enabled at device creation.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_ray_tracing(
            VkDevice device,
            VkPipelineCache cache,
            const VkRayTracingPipelineCreateInfoKHR &info,
            VkDeferredOperationKHR deferred_op = VK_NULL_HANDLE) noexcept {
            if (vkCreateRayTracingPipelinesKHR == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkCreateRayTracingPipelinesKHR is not loaded (ray tracing pipeline extension not enabled).");
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateRayTracingPipelinesKHR(device, deferred_op, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateRayTracingPipelinesKHR failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
            return out;
        }

        // Fetches the shader-group handles a ray tracing pipeline exposes, to be copied into a shader
        // binding table buffer. `handle_data` must be sized `group_count * shader_group_handle_size`
        // (from the device's ray-tracing properties).
        [[nodiscard]] RendererResult get_ray_tracing_shader_group_handles(
            u32 first_group, u32 group_count, span<u8> handle_data) const noexcept {
            if (vkGetRayTracingShaderGroupHandlesKHR == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetRayTracingShaderGroupHandlesKHR is not loaded.");
            if (vkGetRayTracingShaderGroupHandlesKHR(device_, pipeline_, first_group, group_count,
                                                     handle_data.size_bytes(), handle_data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetRayTracingShaderGroupHandlesKHR failed.");
            return {};
        }

        [[nodiscard]] VkPipeline vk_handle() const noexcept { return pipeline_; }
        [[nodiscard]] bool is_valid() const noexcept { return pipeline_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkPipelineBindPoint bind_point() const noexcept { return bind_point_; }
        [[nodiscard]] bool is_graphics() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_GRAPHICS; }
        [[nodiscard]] bool is_compute() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_COMPUTE; }
        [[nodiscard]] bool is_ray_tracing() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR; }

        void destroy() noexcept {
            if (pipeline_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkPipelineBindPoint bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };

    // ─── GraphicsPipelineBuilder ─────────────────────────────────────────────────
    //
    // Assembles the whole fixed-function state block for a dynamic-rendering graphics pipeline behind a
    // fluent API, so callers (and the RHI backend mapping a RenderPipelineDesc) don't hand-roll a dozen
    // Vk*StateCreateInfo structs each time. Every knob has a sensible default (fill/back-cull/CCW, no
    // depth test, no blend, 1 sample, dynamic viewport+scissor); set only what differs from that. All
    // state is owned here and stays alive across the single create() call, so nothing dangles.
    //
    //   auto pipe = GraphicsPipelineBuilder{}
    //       .set_layout(layout)
    //       .add_stage(vert.stage_info()).add_stage(frag.stage_info())
    //       .set_vertex_input(bindings, attributes)
    //       .set_depth_test(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL) // reverse-Z
    //       .add_color_target(VK_FORMAT_R16G16B16A16_SFLOAT)            // HDR G-buffer target
    //       .set_depth_format(VK_FORMAT_D32_SFLOAT)
    //       .create(device, cache);
    class GraphicsPipelineBuilder {
      public:
        GraphicsPipelineBuilder &set_layout(VkPipelineLayout layout) noexcept {
            layout_ = layout;
            return *this;
        }
        GraphicsPipelineBuilder &set_flags(VkPipelineCreateFlags flags) noexcept {
            flags_ = flags;
            return *this;
        }
        // Extra structs to chain after the VkPipelineRenderingCreateInfo this builder always supplies.
        GraphicsPipelineBuilder &set_next(const void *next) noexcept {
            rendering_next_ = next;
            return *this;
        }

        GraphicsPipelineBuilder &add_stage(const VkPipelineShaderStageCreateInfo &stage) {
            stages_.push_back(stage);
            return *this;
        }
        GraphicsPipelineBuilder &set_stages(span<const VkPipelineShaderStageCreateInfo> stages) {
            stages_.assign(stages.begin(), stages.end());
            return *this;
        }

        GraphicsPipelineBuilder &set_mesh_shader_frontend(bool enabled = true) noexcept {
            mesh_shader_frontend_ = enabled;
            return *this;
        }

        GraphicsPipelineBuilder &set_vertex_input(span<const VkVertexInputBindingDescription> bindings,
                                                  span<const VkVertexInputAttributeDescription> attributes) {
            vertex_bindings_.assign(bindings.begin(), bindings.end());
            vertex_attributes_.assign(attributes.begin(), attributes.end());
            return *this;
        }

        GraphicsPipelineBuilder &set_topology(VkPrimitiveTopology topology, bool primitive_restart = false) noexcept {
            topology_ = topology;
            primitive_restart_ = primitive_restart;
            return *this;
        }
        // Tessellation patch size; 0 (default) means the pipeline has no tessellation stages.
        GraphicsPipelineBuilder &set_tessellation_patch_control_points(u32 points) noexcept {
            patch_control_points_ = points;
            return *this;
        }

        GraphicsPipelineBuilder &set_polygon_mode(VkPolygonMode mode) noexcept {
            polygon_mode_ = mode;
            return *this;
        }
        GraphicsPipelineBuilder &set_cull_mode(VkCullModeFlags mode, VkFrontFace front_face) noexcept {
            cull_mode_ = mode;
            front_face_ = front_face;
            return *this;
        }
        GraphicsPipelineBuilder &set_line_width(float width) noexcept {
            line_width_ = width;
            return *this;
        }
        GraphicsPipelineBuilder &set_depth_clamp(bool enable) noexcept {
            depth_clamp_ = enable;
            return *this;
        }
        GraphicsPipelineBuilder &set_rasterizer_discard(bool enable) noexcept {
            rasterizer_discard_ = enable;
            return *this;
        }
        GraphicsPipelineBuilder &set_depth_bias(float constant, float clamp, float slope) noexcept {
            depth_bias_enable_ = true;
            depth_bias_constant_ = constant;
            depth_bias_clamp_ = clamp;
            depth_bias_slope_ = slope;
            return *this;
        }

        GraphicsPipelineBuilder &set_samples(VkSampleCountFlagBits samples, bool alpha_to_coverage = false) noexcept {
            samples_ = samples;
            alpha_to_coverage_ = alpha_to_coverage;
            return *this;
        }
        GraphicsPipelineBuilder &set_sample_mask(u32 mask) noexcept {
            sample_mask_ = mask;
            return *this;
        }

        GraphicsPipelineBuilder &set_depth_test(bool test, bool write,
                                                VkCompareOp compare = VK_COMPARE_OP_LESS) noexcept {
            depth_test_ = test;
            depth_write_ = write;
            depth_compare_ = compare;
            return *this;
        }
        GraphicsPipelineBuilder &set_depth_bounds_test(bool enable) noexcept {
            depth_bounds_test_ = enable;
            return *this;
        }
        GraphicsPipelineBuilder &set_stencil(const VkStencilOpState &front, const VkStencilOpState &back) noexcept {
            stencil_test_ = true;
            stencil_front_ = front;
            stencil_back_ = back;
            return *this;
        }

        // Adds one color target: its dynamic-rendering format plus its blend/write state. Call once per
        // MRT target, in attachment order — this is the multi-render-target G-buffer setup.
        GraphicsPipelineBuilder &add_color_target(VkFormat format,
                                                  const VkPipelineColorBlendAttachmentState &blend) {
            color_formats_.push_back(format);
            blend_attachments_.push_back(blend);
            return *this;
        }
        // Convenience: an opaque (no-blend) target writing all channels.
        GraphicsPipelineBuilder &add_color_target(VkFormat format) {
            return add_color_target(format, VkPipelineColorBlendAttachmentState{
                                                .blendEnable = VK_FALSE,
                                                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                                            });
        }
        GraphicsPipelineBuilder &set_depth_format(VkFormat format) noexcept {
            depth_format_ = format;
            return *this;
        }
        GraphicsPipelineBuilder &set_stencil_format(VkFormat format) noexcept {
            stencil_format_ = format;
            return *this;
        }
        // Multiview view mask (must match the RenderingInfo used at draw time).
        GraphicsPipelineBuilder &set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

        // Replaces the default dynamic state set (viewport + scissor). Pass the full list you want.
        GraphicsPipelineBuilder &set_dynamic_states(span<const VkDynamicState> states) {
            dynamic_states_.assign(states.begin(), states.end());
            return *this;
        }
        GraphicsPipelineBuilder &add_dynamic_state(VkDynamicState state) {
            dynamic_states_.push_back(state);
            return *this;
        }

        [[nodiscard]] RendererExpected<VulkanPipeline> create(VkDevice device,
                                                              VkPipelineCache cache = VK_NULL_HANDLE) const noexcept {
            const VkPipelineVertexInputStateCreateInfo vertex_input{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                .vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings_.size()),
                .pVertexBindingDescriptions = vertex_bindings_.empty() ? nullptr : vertex_bindings_.data(),
                .vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes_.size()),
                .pVertexAttributeDescriptions = vertex_attributes_.empty() ? nullptr : vertex_attributes_.data(),
            };
            const VkPipelineInputAssemblyStateCreateInfo input_assembly{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = topology_,
                .primitiveRestartEnable = primitive_restart_ ? VK_TRUE : VK_FALSE,
            };
            const VkPipelineTessellationStateCreateInfo tessellation{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                .patchControlPoints = patch_control_points_,
            };
            // Viewport/scissor are dynamic by default; counts still need to be valid.
            const VkPipelineViewportStateCreateInfo viewport{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount = 1,
                .scissorCount = 1,
            };
            const VkPipelineRasterizationStateCreateInfo rasterization{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .depthClampEnable = depth_clamp_ ? VK_TRUE : VK_FALSE,
                .rasterizerDiscardEnable = rasterizer_discard_ ? VK_TRUE : VK_FALSE,
                .polygonMode = polygon_mode_,
                .cullMode = cull_mode_,
                .frontFace = front_face_,
                .depthBiasEnable = depth_bias_enable_ ? VK_TRUE : VK_FALSE,
                .depthBiasConstantFactor = depth_bias_constant_,
                .depthBiasClamp = depth_bias_clamp_,
                .depthBiasSlopeFactor = depth_bias_slope_,
                .lineWidth = line_width_,
            };
            const VkPipelineMultisampleStateCreateInfo multisample{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = samples_,
                .sampleShadingEnable = VK_FALSE,
                .pSampleMask = &sample_mask_,
                .alphaToCoverageEnable = alpha_to_coverage_ ? VK_TRUE : VK_FALSE,
            };
            const VkPipelineDepthStencilStateCreateInfo depth_stencil{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .depthTestEnable = depth_test_ ? VK_TRUE : VK_FALSE,
                .depthWriteEnable = depth_write_ ? VK_TRUE : VK_FALSE,
                .depthCompareOp = depth_compare_,
                .depthBoundsTestEnable = depth_bounds_test_ ? VK_TRUE : VK_FALSE,
                .stencilTestEnable = stencil_test_ ? VK_TRUE : VK_FALSE,
                .front = stencil_front_,
                .back = stencil_back_,
                .minDepthBounds = 0.0f,
                .maxDepthBounds = 1.0f,
            };
            const VkPipelineColorBlendStateCreateInfo color_blend{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .logicOpEnable = VK_FALSE,
                .attachmentCount = static_cast<u32>(blend_attachments_.size()),
                .pAttachments = blend_attachments_.empty() ? nullptr : blend_attachments_.data(),
            };
            const VkPipelineDynamicStateCreateInfo dynamic{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .dynamicStateCount = static_cast<u32>(dynamic_states_.size()),
                .pDynamicStates = dynamic_states_.empty() ? nullptr : dynamic_states_.data(),
            };
            const VkPipelineRenderingCreateInfo rendering{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = rendering_next_,
                .viewMask = view_mask_,
                .colorAttachmentCount = static_cast<u32>(color_formats_.size()),
                .pColorAttachmentFormats = color_formats_.empty() ? nullptr : color_formats_.data(),
                .depthAttachmentFormat = depth_format_,
                .stencilAttachmentFormat = stencil_format_,
            };
            const VkGraphicsPipelineCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = &rendering,
                .flags = flags_,
                .stageCount = static_cast<u32>(stages_.size()),
                .pStages = stages_.empty() ? nullptr : stages_.data(),
                .pVertexInputState = mesh_shader_frontend_ ? nullptr : &vertex_input,
                .pInputAssemblyState = mesh_shader_frontend_ ? nullptr : &input_assembly,
                .pTessellationState = patch_control_points_ > 0 ? &tessellation : nullptr,
                .pViewportState = &viewport,
                .pRasterizationState = &rasterization,
                .pMultisampleState = &multisample,
                .pDepthStencilState = &depth_stencil,
                .pColorBlendState = &color_blend,
                .pDynamicState = &dynamic,
                .layout = layout_,
                .renderPass = VK_NULL_HANDLE,
            };
            return VulkanPipeline::create_graphics_dynamic(device, cache, info);
        }

      private:
        vector<VkPipelineShaderStageCreateInfo> stages_;
        vector<VkVertexInputBindingDescription> vertex_bindings_;
        vector<VkVertexInputAttributeDescription> vertex_attributes_;
        vector<VkPipelineColorBlendAttachmentState> blend_attachments_;
        vector<VkFormat> color_formats_;
        vector<VkDynamicState> dynamic_states_{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        bool mesh_shader_frontend_ = false;
        VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
        VkFormat stencil_format_ = VK_FORMAT_UNDEFINED;
        VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        bool primitive_restart_ = false;
        u32 patch_control_points_ = 0;
        VkPolygonMode polygon_mode_ = VK_POLYGON_MODE_FILL;
        VkCullModeFlags cull_mode_ = VK_CULL_MODE_BACK_BIT;
        VkFrontFace front_face_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        float line_width_ = 1.0f;
        bool depth_clamp_ = false;
        bool rasterizer_discard_ = false;
        bool depth_bias_enable_ = false;
        float depth_bias_constant_ = 0.0f;
        float depth_bias_clamp_ = 0.0f;
        float depth_bias_slope_ = 0.0f;
        VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
        bool alpha_to_coverage_ = false;
        u32 sample_mask_ = 0xFFFFFFFFu;
        bool depth_test_ = false;
        bool depth_write_ = false;
        VkCompareOp depth_compare_ = VK_COMPARE_OP_LESS;
        bool depth_bounds_test_ = false;
        bool stencil_test_ = false;
        VkStencilOpState stencil_front_{};
        VkStencilOpState stencil_back_{};
        u32 view_mask_ = 0;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
        VkPipelineCreateFlags flags_ = 0;
        const void *rendering_next_ = nullptr;
    };

    // ─── VulkanPipelineCache ─────────────────────────────────────────────────────

    class VulkanPipelineCache {
      public:
        VulkanPipelineCache() = default;
        ~VulkanPipelineCache() { destroy(); }

        VulkanPipelineCache(const VulkanPipelineCache &) = delete;
        VulkanPipelineCache &operator=(const VulkanPipelineCache &) = delete;

        VulkanPipelineCache(VulkanPipelineCache &&o) noexcept
            : device_(o.device_), cache_(o.cache_) {
            o.device_ = VK_NULL_HANDLE;
            o.cache_ = VK_NULL_HANDLE;
        }
        VulkanPipelineCache &operator=(VulkanPipelineCache &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                cache_ = o.cache_;
                o.device_ = VK_NULL_HANDLE;
                o.cache_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // Pass previously saved cache data to seed the cache; pass an empty span to start fresh.
        [[nodiscard]] static RendererExpected<VulkanPipelineCache> create(
            VkDevice device,
            span<const u8> initial_data = {}) noexcept {
            VkPipelineCacheCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .initialDataSize = initial_data.size_bytes(),
                .pInitialData = initial_data.empty() ? nullptr : initial_data.data(),
            };
            VkPipelineCache cache = VK_NULL_HANDLE;
            if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreatePipelineCache failed.");
            VulkanPipelineCache out;
            out.device_ = device;
            out.cache_ = cache;
            return out;
        }

        [[nodiscard]] VkPipelineCache vk_handle() const noexcept { return cache_; }
        [[nodiscard]] bool is_valid() const noexcept { return cache_ != VK_NULL_HANDLE; }

        // Serializes the cache to a byte blob suitable for saving to disk and re-seeding next run.
        [[nodiscard]] RendererExpected<vector<u8>> serialize() const {
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache_, &size, data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

        void destroy() noexcept {
            if (cache_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineCache(device_, cache_, nullptr);
            cache_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache cache_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
