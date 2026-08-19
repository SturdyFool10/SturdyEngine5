#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {


    class VulkanPipelineLayout {
      public:
        /// Constructs a `VulkanPipelineLayout` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipelineLayout() = default;
        /// Destroys the `VulkanPipelineLayout` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanPipelineLayout();

        /// Disables this construction form for `VulkanPipelineLayout`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipelineLayout(const VulkanPipelineLayout &) = delete;
        /// Assigns a new value to this `VulkanPipelineLayout`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipelineLayout &operator=(const VulkanPipelineLayout &) = delete;

        /// Constructs a `VulkanPipelineLayout` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept;
        /// Assigns a new value to this `VulkanPipelineLayout`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanPipelineLayout &operator=(VulkanPipelineLayout &&o) noexcept;

        /// Creates a `VulkanPipelineLayout` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create(
            VkDevice device,
            const VkPipelineLayoutCreateInfo &info) noexcept;

        /// Creates a from sets from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param set_layouts `set_layouts` value used by the operation.
        /// @param push_constants `push_constants` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_from_sets(
            VkDevice device,
            span<const VkDescriptorSetLayout> set_layouts,
            span<const VkPushConstantRange> push_constants = {}) noexcept;


        /// Creates a empty from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_empty(VkDevice device) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanPipelineLayout`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineLayout vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanPipelineLayout`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Destroys or releases the `VulkanPipelineLayout` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
    };

    class PipelineLayoutBuilder {
      public:
        /// Adds set layout using the supplied arguments and current state.
        ///
        /// @param layout `layout` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineLayoutBuilder &add_set_layout(VkDescriptorSetLayout layout);
        /// Sets the set layouts for this `PipelineLayoutBuilder`.
        ///
        /// @param layouts `layouts` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineLayoutBuilder &set_set_layouts(span<const VkDescriptorSetLayout> layouts);
        /// Adds push constant range using the supplied arguments and current state.
        ///
        /// @param stages `stages` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        PipelineLayoutBuilder &add_push_constant_range(VkShaderStageFlags stages, u32 offset, u32 size);
        /// Creates a `PipelineLayoutBuilder` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanPipelineLayout> create(VkDevice device) const noexcept;

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

        /// Returns the current rendering info.
        ///
        /// @return Returns the current rendering info value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineRenderingCreateInfo rendering_info() const noexcept;
    };


    class VulkanPipeline {
      public:
        /// Constructs a `VulkanPipeline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipeline() = default;
        /// Destroys the `VulkanPipeline` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanPipeline();

        /// Disables this construction form for `VulkanPipeline`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipeline(const VulkanPipeline &) = delete;
        /// Assigns a new value to this `VulkanPipeline`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipeline &operator=(const VulkanPipeline &) = delete;

        /// Constructs a `VulkanPipeline` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipeline(VulkanPipeline &&o) noexcept;
        /// Assigns a new value to this `VulkanPipeline`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanPipeline &operator=(VulkanPipeline &&o) noexcept;


        /// Creates a graphics from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics(
            VkDevice device,
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept;


        /// Creates a graphics dynamic from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics_dynamic(
            VkDevice device,
            VkPipelineCache cache,
            VkGraphicsPipelineCreateInfo info
            ) noexcept;

        /// Creates a compute from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_compute(
            VkDevice device,
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept;


        /// Creates a ray tracing from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param cache `cache` value used by the operation.
        /// @param info Description of the resource or operation to perform.
        /// @param deferred_op `deferred_op` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_ray_tracing(
            VkDevice device,
            VkPipelineCache cache,
            const VkRayTracingPipelineCreateInfoKHR &info,
            VkDeferredOperationKHR deferred_op = VK_NULL_HANDLE) noexcept;


        /// Returns the ray tracing shader group handles associated with this `VulkanPipeline`.
        ///
        /// @param first_group `first_group` value used by the operation.
        /// @param group_count Number of elements or operations to process.
        /// @param handle_data Data consumed or referenced by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult get_ray_tracing_shader_group_handles(
            u32 first_group, u32 group_count, span<u8> handle_data) const noexcept;

        /// Returns the Vulkan handle associated with this `VulkanPipeline`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipeline vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanPipeline`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Binds point for subsequent operations.
        ///
        /// @return Returns the current bind point value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineBindPoint bind_point() const noexcept;
        /// Reports whether graphics holds for this `VulkanPipeline`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_graphics() const noexcept;
        /// Reports whether compute holds for this `VulkanPipeline`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_compute() const noexcept;
        /// Reports whether ray tracing holds for this `VulkanPipeline`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_ray_tracing() const noexcept;

        /// Destroys or releases the `VulkanPipeline` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkPipelineBindPoint bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };


    class GraphicsPipelineBuilder {
      public:
        /// Sets the layout for this `GraphicsPipelineBuilder`.
        ///
        /// @param layout `layout` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_layout(VkPipelineLayout layout) noexcept;
        /// Sets the flags for this `GraphicsPipelineBuilder`.
        ///
        /// @param flags Flags controlling optional behavior.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_flags(VkPipelineCreateFlags flags) noexcept;

        /// Sets the next for this `GraphicsPipelineBuilder`.
        ///
        /// @param next `next` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_next(const void *next) noexcept;

        /// Adds stage using the supplied arguments and current state.
        ///
        /// @param stage `stage` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &add_stage(const VkPipelineShaderStageCreateInfo &stage);
        /// Sets the stages for this `GraphicsPipelineBuilder`.
        ///
        /// @param stages `stages` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &set_stages(span<const VkPipelineShaderStageCreateInfo> stages);

        /// Sets the mesh shader frontend for this `GraphicsPipelineBuilder`.
        ///
        /// @param enabled Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_mesh_shader_frontend(bool enabled = true) noexcept;

        /// Sets the vertex input for this `GraphicsPipelineBuilder`.
        ///
        /// @param bindings `bindings` value used by the operation.
        /// @param attributes `attributes` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &set_vertex_input(span<const VkVertexInputBindingDescription> bindings,
                                                  span<const VkVertexInputAttributeDescription> attributes);

        /// Sets the topology for this `GraphicsPipelineBuilder`.
        ///
        /// @param topology `topology` value used by the operation.
        /// @param primitive_restart `primitive_restart` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_topology(VkPrimitiveTopology topology, bool primitive_restart = false) noexcept;

        /// Sets the tessellation patch control points for this `GraphicsPipelineBuilder`.
        ///
        /// @param points `points` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_tessellation_patch_control_points(u32 points) noexcept;

        /// Sets the polygon mode for this `GraphicsPipelineBuilder`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_polygon_mode(VkPolygonMode mode) noexcept;
        /// Sets the cull mode for this `GraphicsPipelineBuilder`.
        ///
        /// @param mode Mode controlling how the operation is performed.
        /// @param front_face `front_face` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_cull_mode(VkCullModeFlags mode, VkFrontFace front_face) noexcept;
        /// Sets the line width for this `GraphicsPipelineBuilder`.
        ///
        /// @param width Width of the target extent.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_line_width(float width) noexcept;
        /// Sets the depth clamp for this `GraphicsPipelineBuilder`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_depth_clamp(bool enable) noexcept;
        /// Sets the rasterizer discard for this `GraphicsPipelineBuilder`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_rasterizer_discard(bool enable) noexcept;
        /// Sets the depth bias for this `GraphicsPipelineBuilder`.
        ///
        /// @param constant `constant` value used by the operation.
        /// @param clamp `clamp` value used by the operation.
        /// @param slope `slope` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_depth_bias(float constant, float clamp, float slope) noexcept;

        /// Sets the samples for this `GraphicsPipelineBuilder`.
        ///
        /// @param samples `samples` value used by the operation.
        /// @param alpha_to_coverage `alpha_to_coverage` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_samples(VkSampleCountFlagBits samples, bool alpha_to_coverage = false) noexcept;
        /// Sets the sample mask for this `GraphicsPipelineBuilder`.
        ///
        /// @param mask `mask` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_sample_mask(u32 mask) noexcept;

        /// Sets the depth test for this `GraphicsPipelineBuilder`.
        ///
        /// @param test `test` value used by the operation.
        /// @param write `write` value used by the operation.
        /// @param compare `compare` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_depth_test(bool test, bool write,
                                                VkCompareOp compare = VK_COMPARE_OP_LESS) noexcept;
        /// Sets the depth bounds test for this `GraphicsPipelineBuilder`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_depth_bounds_test(bool enable) noexcept;
        /// Sets conservative rasterization (overestimation mode) for this `GraphicsPipelineBuilder`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_conservative_rasterization(bool enable) noexcept;
        /// Opts this pipeline into RenderPassEncoder::set_sample_locations() for this `GraphicsPipelineBuilder`.
        ///
        /// @param enable Whether the associated behavior is enabled.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_sample_locations_enable(bool enable) noexcept;
        /// Sets the stencil for this `GraphicsPipelineBuilder`.
        ///
        /// @param front `front` value used by the operation.
        /// @param back `back` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_stencil(const VkStencilOpState &front, const VkStencilOpState &back) noexcept;


        /// Adds color target using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param blend `blend` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &add_color_target(VkFormat format,
                                                  const VkPipelineColorBlendAttachmentState &blend);

        /// Adds color target using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &add_color_target(VkFormat format);
        /// Sets the depth format for this `GraphicsPipelineBuilder`.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_depth_format(VkFormat format) noexcept;
        /// Sets the stencil format for this `GraphicsPipelineBuilder`.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_stencil_format(VkFormat format) noexcept;

        /// Sets the view mask for this `GraphicsPipelineBuilder`.
        ///
        /// @param mask `mask` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        GraphicsPipelineBuilder &set_view_mask(u32 mask) noexcept;


        /// Sets the dynamic states for this `GraphicsPipelineBuilder`.
        ///
        /// @param states `states` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &set_dynamic_states(span<const VkDynamicState> states);
        /// Adds dynamic state using the supplied arguments and current state.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        GraphicsPipelineBuilder &add_dynamic_state(VkDynamicState state);

        /// Creates a `GraphicsPipelineBuilder` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param cache `cache` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<VulkanPipeline> create(VkDevice device,
                                                              VkPipelineCache cache = VK_NULL_HANDLE) const noexcept;

      private:
        vector<VkPipelineShaderStageCreateInfo> stages_;
        vector<VkVertexInputBindingDescription> vertex_bindings_;
        vector<VkVertexInputAttributeDescription> vertex_attributes_;
        vector<VkPipelineColorBlendAttachmentState> blend_attachments_;
        vector<VkFormat> color_formats_;
        vector<VkDynamicState> dynamic_states_{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };

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
        bool conservative_rasterization_ = false;
        bool sample_locations_enable_ = false;
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


    class VulkanPipelineCache {
      public:
        /// Constructs a `VulkanPipelineCache` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipelineCache() = default;
        /// Destroys the `VulkanPipelineCache` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanPipelineCache();

        /// Disables this construction form for `VulkanPipelineCache`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipelineCache(const VulkanPipelineCache &) = delete;
        /// Assigns a new value to this `VulkanPipelineCache`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanPipelineCache &operator=(const VulkanPipelineCache &) = delete;

        /// Constructs a `VulkanPipelineCache` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanPipelineCache(VulkanPipelineCache &&o) noexcept;
        /// Assigns a new value to this `VulkanPipelineCache`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanPipelineCache &operator=(VulkanPipelineCache &&o) noexcept;


        /// Creates a `VulkanPipelineCache` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param initial_data Data consumed or referenced by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanPipelineCache> create(
            VkDevice device,
            span<const u8> initial_data = {}) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanPipelineCache`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPipelineCache vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanPipelineCache`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;


        /// Returns the current or globally available serialize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<vector<u8>> serialize() const;

        /// Destroys or releases the `VulkanPipelineCache` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache cache_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
