#include "VulkanPipeline.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanPipelineLayout::~VulkanPipelineLayout() { destroy(); }

/// Performs the vulkan pipeline layout operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanPipelineLayout::VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            ZoneScopedN("VulkanPipelineLayout::VulkanPipelineLayout");
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanPipelineLayout &VulkanPipelineLayout::operator=(VulkanPipelineLayout &&o) noexcept {
            ZoneScopedN("VulkanPipelineLayout::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create(
            VkDevice device,
            const VkPipelineLayoutCreateInfo &info) noexcept {
            ZoneScopedN("VulkanPipelineLayout::create");
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreatePipelineLayout failed.");
            VulkanPipelineLayout out;
            out.device_ = device;
            out.layout_ = layout;
            return out;
        }

/// Creates a from sets from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param set_layouts `set_layouts` value used by the operation.
/// @param push_constants `push_constants` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create_from_sets(
            VkDevice device,
            span<const VkDescriptorSetLayout> set_layouts,
            span<const VkPushConstantRange> push_constants) noexcept {
            ZoneScopedN("VulkanPipelineLayout::create_from_sets");
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

/// Creates a empty from the supplied parameters.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipelineLayout> VulkanPipelineLayout::create_empty(VkDevice device) noexcept {
            ZoneScopedN("VulkanPipelineLayout::create_empty");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineLayout VulkanPipelineLayout::vk_handle() const noexcept { return layout_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipelineLayout::is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanPipelineLayout::destroy() noexcept {
            ZoneScopedN("VulkanPipelineLayout::destroy");
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Adds set layout using the supplied arguments and current state.
///
/// @param layout `layout` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineLayoutBuilder &PipelineLayoutBuilder::add_set_layout(VkDescriptorSetLayout layout) {
            ZoneScopedN("PipelineLayoutBuilder::add_set_layout");
            set_layouts_.push_back(layout);
            return *this;
        }

/// Sets the set layouts for this `Vulkan`.
///
/// @param layouts `layouts` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineLayoutBuilder &PipelineLayoutBuilder::set_set_layouts(span<const VkDescriptorSetLayout> layouts) {
            ZoneScopedN("PipelineLayoutBuilder::set_set_layouts");
            set_layouts_.assign(layouts.begin(), layouts.end());
            return *this;
        }

/// Adds push constant range using the supplied arguments and current state.
///
/// @param stages `stages` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
PipelineLayoutBuilder &PipelineLayoutBuilder::add_push_constant_range(VkShaderStageFlags stages, u32 offset, u32 size) {
            ZoneScopedN("PipelineLayoutBuilder::add_push_constant_range");
            push_constants_.push_back(VkPushConstantRange{
                .stageFlags = stages,
                .offset = offset,
                .size = size,
            });
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipelineLayout> PipelineLayoutBuilder::create(VkDevice device) const noexcept {
            ZoneScopedN("PipelineLayoutBuilder::create");
            return VulkanPipelineLayout::create_from_sets(device, set_layouts_, push_constants_);
        }

/// Returns the current rendering info.
///
/// @return Returns the current rendering info value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineRenderingCreateInfo VulkanGraphicsPipelineSignature::rendering_info() const noexcept {
            ZoneScopedN("VulkanGraphicsPipelineSignature::rendering_info");
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

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanPipeline::~VulkanPipeline() { destroy(); }

/// Performs the vulkan pipeline operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanPipeline::VulkanPipeline(VulkanPipeline &&o) noexcept
            : device_(o.device_), pipeline_(o.pipeline_), bind_point_(o.bind_point_) {
            ZoneScopedN("VulkanPipeline::VulkanPipeline");
            o.device_ = VK_NULL_HANDLE;
            o.pipeline_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanPipeline &VulkanPipeline::operator=(VulkanPipeline &&o) noexcept {
            ZoneScopedN("VulkanPipeline::operator=");
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

/// Creates a graphics from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_graphics(
            VkDevice device,
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            ZoneScopedN("VulkanPipeline::create_graphics");
            if (device == VK_NULL_HANDLE)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines called with a null VkDevice.");
            if (vkCreateGraphicsPipelines == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines is not loaded. Call volkLoadDevice after device creation.");

            const Foundation::Stopwatch stopwatch;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines failed.");
            Foundation::log_info("Vulkan: created graphics pipeline ({} stage(s)) in {}", info.stageCount, stopwatch.elapsed_human());
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

/// Creates a graphics dynamic from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_graphics_dynamic(
            VkDevice device,
            VkPipelineCache cache,
            VkGraphicsPipelineCreateInfo info
            ) noexcept {
            ZoneScopedN("VulkanPipeline::create_graphics_dynamic");
            if (device == VK_NULL_HANDLE) [[unlikely]] {
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    "vkCreateGraphicsPipelines (dynamic rendering) called with a null VkDevice.");
            }
            if (vkCreateGraphicsPipelines == nullptr) [[unlikely]] {
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    "vkCreateGraphicsPipelines is not loaded. Call volkLoadDevice after device creation.");
            }
            if (info.renderPass != VK_NULL_HANDLE) [[unlikely]] {
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    "Dynamic-rendering graphics pipelines must have a null VkGraphicsPipelineCreateInfo::renderPass.");
            }
            const Foundation::Stopwatch stopwatch;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkCreateGraphicsPipelines (dynamic rendering) failed.");
            Foundation::log_info("Vulkan: created graphics pipeline (dynamic rendering, {} stage(s)) in {}", info.stageCount, stopwatch.elapsed_human());
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

/// Creates a compute from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_compute(
            VkDevice device,
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            ZoneScopedN("VulkanPipeline::create_compute");
            const Foundation::Stopwatch stopwatch;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateComputePipelines failed.");
            Foundation::log_info("Vulkan: created compute pipeline in {}", stopwatch.elapsed_human());
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_COMPUTE;
            return out;
        }

/// Creates a ray tracing from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
/// @param deferred_op `deferred_op` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipeline> VulkanPipeline::create_ray_tracing(
            VkDevice device,
            VkPipelineCache cache,
            const VkRayTracingPipelineCreateInfoKHR &info,
            VkDeferredOperationKHR deferred_op) noexcept {
            ZoneScopedN("VulkanPipeline::create_ray_tracing");
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

/// Returns the ray tracing shader group handles associated with this `Vulkan`.
///
/// @param first_group `first_group` value used by the operation.
/// @param group_count Number of elements or operations to process.
/// @param handle_data Data consumed or referenced by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanPipeline::get_ray_tracing_shader_group_handles(
            u32 first_group, u32 group_count, span<u8> handle_data) const noexcept {
            ZoneScopedN("VulkanPipeline::get_ray_tracing_shader_group_handles");
            if (vkGetRayTracingShaderGroupHandlesKHR == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetRayTracingShaderGroupHandlesKHR is not loaded.");
            if (vkGetRayTracingShaderGroupHandlesKHR(device_, pipeline_, first_group, group_count,
                                                     handle_data.size_bytes(), handle_data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetRayTracingShaderGroupHandlesKHR failed.");
            return {};
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipeline VulkanPipeline::vk_handle() const noexcept { return pipeline_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipeline::is_valid() const noexcept { return pipeline_ != VK_NULL_HANDLE; }

/// Binds point for subsequent operations.
///
/// @return Returns the current bind point value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineBindPoint VulkanPipeline::bind_point() const noexcept { return bind_point_; }

/// Reports whether graphics holds for this `Vulkan`.
///
/// @return Returns the current is graphics value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipeline::is_graphics() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_GRAPHICS; }

/// Reports whether compute holds for this `Vulkan`.
///
/// @return Returns the current is compute value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipeline::is_compute() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_COMPUTE; }

/// Reports whether ray tracing holds for this `Vulkan`.
///
/// @return Returns the current is ray tracing value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipeline::is_ray_tracing() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanPipeline::destroy() noexcept {
            ZoneScopedN("VulkanPipeline::destroy");
            if (pipeline_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

/// Sets the layout for this `Vulkan`.
///
/// @param layout `layout` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_layout(VkPipelineLayout layout) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_layout");
            layout_ = layout;
            return *this;
        }

/// Sets the flags for this `Vulkan`.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_flags(VkPipelineCreateFlags flags) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_flags");
            flags_ = flags;
            return *this;
        }

/// Sets the next for this `Vulkan`.
///
/// @param next `next` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_next(const void *next) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_next");
            rendering_next_ = next;
            return *this;
        }

/// Adds stage using the supplied arguments and current state.
///
/// @param stage `stage` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_stage(const VkPipelineShaderStageCreateInfo &stage) {
            ZoneScopedN("GraphicsPipelineBuilder::add_stage");
            stages_.push_back(stage);
            return *this;
        }

/// Sets the stages for this `Vulkan`.
///
/// @param stages `stages` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stages(span<const VkPipelineShaderStageCreateInfo> stages) {
            ZoneScopedN("GraphicsPipelineBuilder::set_stages");
            stages_.assign(stages.begin(), stages.end());
            return *this;
        }

/// Sets the mesh shader frontend for this `Vulkan`.
///
/// @param enabled Whether the associated behavior is enabled.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_mesh_shader_frontend(bool enabled) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_mesh_shader_frontend");
            mesh_shader_frontend_ = enabled;
            return *this;
        }

/// Sets the vertex input for this `Vulkan`.
///
/// @param bindings `bindings` value used by the operation.
/// @param attributes `attributes` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_vertex_input(span<const VkVertexInputBindingDescription> bindings,
                                                  span<const VkVertexInputAttributeDescription> attributes) {
            ZoneScopedN("GraphicsPipelineBuilder::set_vertex_input");
            vertex_bindings_.assign(bindings.begin(), bindings.end());
            vertex_attributes_.assign(attributes.begin(), attributes.end());
            return *this;
        }

/// Sets the topology for this `Vulkan`.
///
/// @param topology `topology` value used by the operation.
/// @param primitive_restart `primitive_restart` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_topology(VkPrimitiveTopology topology, bool primitive_restart) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_topology");
            topology_ = topology;
            primitive_restart_ = primitive_restart;
            return *this;
        }

/// Sets the tessellation patch control points for this `Vulkan`.
///
/// @param points `points` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_tessellation_patch_control_points(u32 points) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_tessellation_patch_control_points");
            patch_control_points_ = points;
            return *this;
        }

/// Sets the polygon mode for this `Vulkan`.
///
/// @param mode Mode controlling how the operation is performed.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_polygon_mode(VkPolygonMode mode) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_polygon_mode");
            polygon_mode_ = mode;
            return *this;
        }

/// Sets the cull mode for this `Vulkan`.
///
/// @param mode Mode controlling how the operation is performed.
/// @param front_face `front_face` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_cull_mode(VkCullModeFlags mode, VkFrontFace front_face) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_cull_mode");
            cull_mode_ = mode;
            front_face_ = front_face;
            return *this;
        }

/// Sets the line width for this `Vulkan`.
///
/// @param width Width of the target extent.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_line_width(float width) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_line_width");
            line_width_ = width;
            return *this;
        }

/// Sets the depth clamp for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_clamp(bool enable) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_depth_clamp");
            depth_clamp_ = enable;
            return *this;
        }

/// Sets the rasterizer discard for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_rasterizer_discard(bool enable) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_rasterizer_discard");
            rasterizer_discard_ = enable;
            return *this;
        }

/// Sets the depth bias for this `Vulkan`.
///
/// @param constant `constant` value used by the operation.
/// @param clamp `clamp` value used by the operation.
/// @param slope `slope` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_bias(float constant, float clamp, float slope) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_depth_bias");
            depth_bias_enable_ = true;
            depth_bias_constant_ = constant;
            depth_bias_clamp_ = clamp;
            depth_bias_slope_ = slope;
            return *this;
        }

/// Sets the samples for this `Vulkan`.
///
/// @param samples `samples` value used by the operation.
/// @param alpha_to_coverage `alpha_to_coverage` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_samples(VkSampleCountFlagBits samples, bool alpha_to_coverage) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_samples");
            samples_ = samples;
            alpha_to_coverage_ = alpha_to_coverage;
            return *this;
        }

/// Sets the sample mask for this `Vulkan`.
///
/// @param mask `mask` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_sample_mask(u32 mask) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_sample_mask");
            sample_mask_ = mask;
            return *this;
        }

/// Sets the depth test for this `Vulkan`.
///
/// @param test `test` value used by the operation.
/// @param write `write` value used by the operation.
/// @param compare `compare` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_test(bool test, bool write,
                                                VkCompareOp compare) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_depth_test");
            depth_test_ = test;
            depth_write_ = write;
            depth_compare_ = compare;
            return *this;
        }

/// Sets the depth bounds test for this `Vulkan`.
///
/// @param enable Whether the associated behavior is enabled.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_bounds_test(bool enable) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_depth_bounds_test");
            depth_bounds_test_ = enable;
            return *this;
        }

/// Sets the stencil for this `Vulkan`.
///
/// @param front `front` value used by the operation.
/// @param back `back` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stencil(const VkStencilOpState &front, const VkStencilOpState &back) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_stencil");
            stencil_test_ = true;
            stencil_front_ = front;
            stencil_back_ = back;
            return *this;
        }

/// Adds color target using the supplied arguments and current state.
///
/// @param format Format used for the resource, render target, or conversion.
/// @param blend `blend` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_color_target(VkFormat format,
                                                  const VkPipelineColorBlendAttachmentState &blend) {
            ZoneScopedN("GraphicsPipelineBuilder::add_color_target");
            color_formats_.push_back(format);
            blend_attachments_.push_back(blend);
            return *this;
        }

/// Adds color target using the supplied arguments and current state.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_color_target(VkFormat format) {
            ZoneScopedN("GraphicsPipelineBuilder::add_color_target");
            return add_color_target(format, VkPipelineColorBlendAttachmentState{
                                                .blendEnable = VK_FALSE,
                                                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                                            });
        }

/// Sets the depth format for this `Vulkan`.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_depth_format(VkFormat format) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_depth_format");
            depth_format_ = format;
            return *this;
        }

/// Sets the stencil format for this `Vulkan`.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_stencil_format(VkFormat format) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_stencil_format");
            stencil_format_ = format;
            return *this;
        }

/// Sets the view mask for this `Vulkan`.
///
/// @param mask `mask` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_view_mask(u32 mask) noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::set_view_mask");
            view_mask_ = mask;
            return *this;
        }

/// Sets the dynamic states for this `Vulkan`.
///
/// @param states `states` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::set_dynamic_states(span<const VkDynamicState> states) {
            ZoneScopedN("GraphicsPipelineBuilder::set_dynamic_states");
            dynamic_states_.assign(states.begin(), states.end());
            return *this;
        }

/// Adds dynamic state using the supplied arguments and current state.
///
/// @param state `state` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
GraphicsPipelineBuilder &GraphicsPipelineBuilder::add_dynamic_state(VkDynamicState state) {
            ZoneScopedN("GraphicsPipelineBuilder::add_dynamic_state");
            dynamic_states_.push_back(state);
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param cache `cache` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipeline> GraphicsPipelineBuilder::create(VkDevice device,
                                                              VkPipelineCache cache) const noexcept {
            ZoneScopedN("GraphicsPipelineBuilder::create");
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


            const bool has_depth_stencil_attachment =
                depth_format_ != VK_FORMAT_UNDEFINED || stencil_format_ != VK_FORMAT_UNDEFINED;
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
                .pDepthStencilState = has_depth_stencil_attachment ? &depth_stencil : nullptr,
                .pColorBlendState = &color_blend,
                .pDynamicState = &dynamic,
                .layout = layout_,
                .renderPass = VK_NULL_HANDLE,
            };
            return VulkanPipeline::create_graphics_dynamic(device, cache, info);
        }

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanPipelineCache::~VulkanPipelineCache() { destroy(); }

/// Performs the vulkan pipeline cache operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanPipelineCache::VulkanPipelineCache(VulkanPipelineCache &&o) noexcept
            : device_(o.device_), cache_(o.cache_) {
            ZoneScopedN("VulkanPipelineCache::VulkanPipelineCache");
            o.device_ = VK_NULL_HANDLE;
            o.cache_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanPipelineCache &VulkanPipelineCache::operator=(VulkanPipelineCache &&o) noexcept {
            ZoneScopedN("VulkanPipelineCache::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                cache_ = o.cache_;
                o.device_ = VK_NULL_HANDLE;
                o.cache_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param initial_data Data consumed or referenced by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanPipelineCache> VulkanPipelineCache::create(
            VkDevice device,
            span<const u8> initial_data) noexcept {
            ZoneScopedN("VulkanPipelineCache::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPipelineCache VulkanPipelineCache::vk_handle() const noexcept { return cache_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPipelineCache::is_valid() const noexcept { return cache_ != VK_NULL_HANDLE; }

/// Returns the current or globally available serialize value.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<u8>> VulkanPipelineCache::serialize() const {
            ZoneScopedN("VulkanPipelineCache::serialize");
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache_, &size, data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanPipelineCache::destroy() noexcept {
            ZoneScopedN("VulkanPipelineCache::destroy");
            if (cache_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineCache(device_, cache_, nullptr);
            cache_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
