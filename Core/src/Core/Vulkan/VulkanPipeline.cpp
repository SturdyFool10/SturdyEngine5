#include "VulkanPipeline.hpp"

namespace SFT::Core::Vulkan {

VulkanPipelineLayout::~VulkanPipelineLayout() { destroy(); }

VulkanPipelineLayout::VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }

VulkanPipelineLayout &VulkanPipelineLayout::operator=(VulkanPipelineLayout &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create(
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

[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create_from_sets(
            VkDevice device,
            span<const VkDescriptorSetLayout> set_layouts,
            span<const VkPushConstantRange> push_constants) noexcept {
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

[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create_empty(VkDevice device) noexcept {
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

[[nodiscard]] VkPipelineLayout VulkanPipelineLayout::vk_handle() const noexcept { return layout_; }

[[nodiscard]] bool VulkanPipelineLayout::is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

void VulkanPipelineLayout::destroy() noexcept {
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

PipelineLayoutBuilder &PipelineLayoutBuilder::add_set_layout(VkDescriptorSetLayout layout) {
            set_layouts_.push_back(layout);
            return *this;
        }

PipelineLayoutBuilder &PipelineLayoutBuilder::set_set_layouts(span<const VkDescriptorSetLayout> layouts) {
            set_layouts_.assign(layouts.begin(), layouts.end());
            return *this;
        }

PipelineLayoutBuilder &PipelineLayoutBuilder::add_push_constant_range(VkShaderStageFlags stages, u32 offset, u32 size) {
            push_constants_.push_back(VkPushConstantRange{
                .stageFlags = stages,
                .offset = offset,
                .size = size,
            });
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanPipelineLayout> PipelineLayoutBuilder::create(VkDevice device) const noexcept {
            return VulkanPipelineLayout::create_from_sets(device, set_layouts_, push_constants_);
        }

[[nodiscard]] VkPipelineRenderingCreateInfo VulkanGraphicsPipelineSignature::rendering_info() const noexcept {
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

VulkanPipeline::~VulkanPipeline() { destroy(); }

VulkanPipeline::VulkanPipeline(VulkanPipeline &&o) noexcept
            : device_(o.device_), pipeline_(o.pipeline_), bind_point_(o.bind_point_) {
            o.device_ = VK_NULL_HANDLE;
            o.pipeline_ = VK_NULL_HANDLE;
        }

VulkanPipeline &VulkanPipeline::operator=(VulkanPipeline &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_graphics(
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

[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_graphics_dynamic(
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

[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_compute(
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

[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_ray_tracing(
            VkDevice device,
            VkPipelineCache cache,
            const VkRayTracingPipelineCreateInfoKHR &info,
            VkDeferredOperationKHR deferred_op) noexcept {
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

[[nodiscard]] RendererResult VulkanPipeline::get_ray_tracing_shader_group_handles(
            u32 first_group, u32 group_count, span<u8> handle_data) const noexcept {
            if (vkGetRayTracingShaderGroupHandlesKHR == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetRayTracingShaderGroupHandlesKHR is not loaded.");
            if (vkGetRayTracingShaderGroupHandlesKHR(device_, pipeline_, first_group, group_count,
                                                     handle_data.size_bytes(), handle_data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetRayTracingShaderGroupHandlesKHR failed.");
            return {};
        }

[[nodiscard]] VkPipeline VulkanPipeline::vk_handle() const noexcept { return pipeline_; }

[[nodiscard]] bool VulkanPipeline::is_valid() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

[[nodiscard]] VkPipelineBindPoint VulkanPipeline::bind_point() const noexcept { return bind_point_; }

[[nodiscard]] bool VulkanPipeline::is_graphics() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_GRAPHICS; }

[[nodiscard]] bool VulkanPipeline::is_compute() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_COMPUTE; }

[[nodiscard]] bool VulkanPipeline::is_ray_tracing() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR; }

void VulkanPipeline::destroy() noexcept {
            if (pipeline_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_layout(VkPipelineLayout layout) noexcept {
            layout_ = layout;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_flags(VkPipelineCreateFlags flags) noexcept {
            flags_ = flags;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_next(const void *next) noexcept {
            rendering_next_ = next;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_stage(const VkPipelineShaderStageCreateInfo &stage) {
            stages_.push_back(stage);
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stages(span<const VkPipelineShaderStageCreateInfo> stages) {
            stages_.assign(stages.begin(), stages.end());
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_mesh_shader_frontend(bool enabled) noexcept {
            mesh_shader_frontend_ = enabled;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_vertex_input(span<const VkVertexInputBindingDescription> bindings,
                                                  span<const VkVertexInputAttributeDescription> attributes) {
            vertex_bindings_.assign(bindings.begin(), bindings.end());
            vertex_attributes_.assign(attributes.begin(), attributes.end());
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_topology(VkPrimitiveTopology topology, bool primitive_restart) noexcept {
            topology_ = topology;
            primitive_restart_ = primitive_restart;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_tessellation_patch_control_points(u32 points) noexcept {
            patch_control_points_ = points;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_polygon_mode(VkPolygonMode mode) noexcept {
            polygon_mode_ = mode;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_cull_mode(VkCullModeFlags mode, VkFrontFace front_face) noexcept {
            cull_mode_ = mode;
            front_face_ = front_face;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_line_width(float width) noexcept {
            line_width_ = width;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_clamp(bool enable) noexcept {
            depth_clamp_ = enable;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_rasterizer_discard(bool enable) noexcept {
            rasterizer_discard_ = enable;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_bias(float constant, float clamp, float slope) noexcept {
            depth_bias_enable_ = true;
            depth_bias_constant_ = constant;
            depth_bias_clamp_ = clamp;
            depth_bias_slope_ = slope;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_samples(VkSampleCountFlagBits samples, bool alpha_to_coverage) noexcept {
            samples_ = samples;
            alpha_to_coverage_ = alpha_to_coverage;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_sample_mask(u32 mask) noexcept {
            sample_mask_ = mask;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_test(bool test, bool write,
                                                VkCompareOp compare) noexcept {
            depth_test_ = test;
            depth_write_ = write;
            depth_compare_ = compare;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_bounds_test(bool enable) noexcept {
            depth_bounds_test_ = enable;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stencil(const VkStencilOpState &front, const VkStencilOpState &back) noexcept {
            stencil_test_ = true;
            stencil_front_ = front;
            stencil_back_ = back;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_color_target(VkFormat format,
                                                  const VkPipelineColorBlendAttachmentState &blend) {
            color_formats_.push_back(format);
            blend_attachments_.push_back(blend);
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_color_target(VkFormat format) {
            return add_color_target(format, VkPipelineColorBlendAttachmentState{
                                                .blendEnable = VK_FALSE,
                                                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                                            });
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_format(VkFormat format) noexcept {
            depth_format_ = format;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stencil_format(VkFormat format) noexcept {
            stencil_format_ = format;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_view_mask(u32 mask) noexcept {
            view_mask_ = mask;
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_dynamic_states(span<const VkDynamicState> states) {
            dynamic_states_.assign(states.begin(), states.end());
            return *this;
        }

GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_dynamic_state(VkDynamicState state) {
            dynamic_states_.push_back(state);
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanPipeline> GraphicsPipelineBuilder::create(VkDevice device,
                                                              VkPipelineCache cache) const noexcept {
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

VulkanPipelineCache::~VulkanPipelineCache() { destroy(); }

VulkanPipelineCache::VulkanPipelineCache(VulkanPipelineCache &&o) noexcept
            : device_(o.device_), cache_(o.cache_) {
            o.device_ = VK_NULL_HANDLE;
            o.cache_ = VK_NULL_HANDLE;
        }

VulkanPipelineCache &VulkanPipelineCache::operator=(VulkanPipelineCache &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                cache_ = o.cache_;
                o.device_ = VK_NULL_HANDLE;
                o.cache_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanPipelineCache> VulkanPipelineCache::create(
            VkDevice device,
            span<const u8> initial_data) noexcept {
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

[[nodiscard]] VkPipelineCache VulkanPipelineCache::vk_handle() const noexcept { return cache_; }

[[nodiscard]] bool VulkanPipelineCache::is_valid() const noexcept { return cache_ != VK_NULL_HANDLE; }

[[nodiscard]] RendererExpected<vector<u8>> VulkanPipelineCache::serialize() const {
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache_, &size, data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

void VulkanPipelineCache::destroy() noexcept {
            if (cache_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineCache(device_, cache_, nullptr);
            cache_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
