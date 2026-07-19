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

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    // ─── VulkanPipelineLayout ────────────────────────────────────────────────────

    class VulkanPipelineLayout {
      public:
        VulkanPipelineLayout() = default;
        ~VulkanPipelineLayout();

        VulkanPipelineLayout(const VulkanPipelineLayout &) = delete;
        VulkanPipelineLayout &operator=(const VulkanPipelineLayout &) = delete;

        VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept;
        VulkanPipelineLayout &operator=(VulkanPipelineLayout &&o) noexcept;

        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create(
            VkDevice device,
            const VkPipelineLayoutCreateInfo &info) noexcept;

        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_from_sets(
            VkDevice device,
            span<const VkDescriptorSetLayout> set_layouts,
            span<const VkPushConstantRange> push_constants = {}) noexcept;

        // Convenience: empty layout (no push constants, no descriptor sets).
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_empty(VkDevice device) noexcept;

        [[nodiscard]] VkPipelineLayout vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
    };

    class PipelineLayoutBuilder {
      public:
        PipelineLayoutBuilder &add_set_layout(VkDescriptorSetLayout layout);
        PipelineLayoutBuilder &set_set_layouts(span<const VkDescriptorSetLayout> layouts);
        PipelineLayoutBuilder &add_push_constant_range(VkShaderStageFlags stages, u32 offset, u32 size);
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

        [[nodiscard]] VkPipelineRenderingCreateInfo rendering_info() const noexcept;
    };

    // ─── VulkanPipeline ──────────────────────────────────────────────────────────

    class VulkanPipeline {
      public:
        VulkanPipeline() = default;
        ~VulkanPipeline();

        VulkanPipeline(const VulkanPipeline &) = delete;
        VulkanPipeline &operator=(const VulkanPipeline &) = delete;

        VulkanPipeline(VulkanPipeline &&o) noexcept;
        VulkanPipeline &operator=(VulkanPipeline &&o) noexcept;

        // For pipelines used with traditional render passes.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics(
            VkDevice device,
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept;

        // For pipelines used with vkCmdBeginRendering (Vulkan 1.3+ dynamic rendering).
        // Set info.renderPass = VK_NULL_HANDLE and chain a VkPipelineRenderingCreateInfo
        // (from PipelineRenderingInfo::to_vk() or VulkanGraphicsPipelineSignature::rendering_info())
        // into info.pNext describing the attachment formats.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics_dynamic(
            VkDevice device,
            VkPipelineCache cache,
            VkGraphicsPipelineCreateInfo info // taken by value so we can assert renderPass is null
            ) noexcept;

        [[nodiscard]] static RendererExpected<VulkanPipeline> create_compute(
            VkDevice device,
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept;

        // Ray tracing pipeline (RayTracingPipeline feature). `deferred_op` may be VK_NULL_HANDLE for a
        // blocking build, or a VkDeferredOperationKHR to offload compilation onto worker threads
        // (DeferredHostOperations). Function-pointer-guarded — the entry point is null unless the
        // ray-tracing-pipeline extension was enabled at device creation.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_ray_tracing(
            VkDevice device,
            VkPipelineCache cache,
            const VkRayTracingPipelineCreateInfoKHR &info,
            VkDeferredOperationKHR deferred_op = VK_NULL_HANDLE) noexcept;

        // Fetches the shader-group handles a ray tracing pipeline exposes, to be copied into a shader
        // binding table buffer. `handle_data` must be sized `group_count * shader_group_handle_size`
        // (from the device's ray-tracing properties).
        [[nodiscard]] RendererResult get_ray_tracing_shader_group_handles(
            u32 first_group, u32 group_count, span<u8> handle_data) const noexcept;

        [[nodiscard]] VkPipeline vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkPipelineBindPoint bind_point() const noexcept;
        [[nodiscard]] bool is_graphics() const noexcept;
        [[nodiscard]] bool is_compute() const noexcept;
        [[nodiscard]] bool is_ray_tracing() const noexcept;

        void destroy() noexcept;

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
        GraphicsPipelineBuilder &set_layout(VkPipelineLayout layout) noexcept;
        GraphicsPipelineBuilder &set_flags(VkPipelineCreateFlags flags) noexcept;
        // Extra structs to chain after the VkPipelineRenderingCreateInfo this builder always supplies.
        GraphicsPipelineBuilder &set_next(const void *next) noexcept;

        GraphicsPipelineBuilder &add_stage(const VkPipelineShaderStageCreateInfo &stage);
        GraphicsPipelineBuilder &set_stages(span<const VkPipelineShaderStageCreateInfo> stages);

        GraphicsPipelineBuilder &set_mesh_shader_frontend(bool enabled = true) noexcept;

        GraphicsPipelineBuilder &set_vertex_input(span<const VkVertexInputBindingDescription> bindings,
                                                  span<const VkVertexInputAttributeDescription> attributes);

        GraphicsPipelineBuilder &set_topology(VkPrimitiveTopology topology, bool primitive_restart = false) noexcept;
        // Tessellation patch size; 0 (default) means the pipeline has no tessellation stages.
        GraphicsPipelineBuilder &set_tessellation_patch_control_points(u32 points) noexcept;

        GraphicsPipelineBuilder &set_polygon_mode(VkPolygonMode mode) noexcept;
        GraphicsPipelineBuilder &set_cull_mode(VkCullModeFlags mode, VkFrontFace front_face) noexcept;
        GraphicsPipelineBuilder &set_line_width(float width) noexcept;
        GraphicsPipelineBuilder &set_depth_clamp(bool enable) noexcept;
        GraphicsPipelineBuilder &set_rasterizer_discard(bool enable) noexcept;
        GraphicsPipelineBuilder &set_depth_bias(float constant, float clamp, float slope) noexcept;

        GraphicsPipelineBuilder &set_samples(VkSampleCountFlagBits samples, bool alpha_to_coverage = false) noexcept;
        GraphicsPipelineBuilder &set_sample_mask(u32 mask) noexcept;

        GraphicsPipelineBuilder &set_depth_test(bool test, bool write,
                                                VkCompareOp compare = VK_COMPARE_OP_LESS) noexcept;
        GraphicsPipelineBuilder &set_depth_bounds_test(bool enable) noexcept;
        GraphicsPipelineBuilder &set_stencil(const VkStencilOpState &front, const VkStencilOpState &back) noexcept;

        // Adds one color target: its dynamic-rendering format plus its blend/write state. Call once per
        // MRT target, in attachment order — this is the multi-render-target G-buffer setup.
        GraphicsPipelineBuilder &add_color_target(VkFormat format,
                                                  const VkPipelineColorBlendAttachmentState &blend);
        // Convenience: an opaque (no-blend) target writing all channels.
        GraphicsPipelineBuilder &add_color_target(VkFormat format);
        GraphicsPipelineBuilder &set_depth_format(VkFormat format) noexcept;
        GraphicsPipelineBuilder &set_stencil_format(VkFormat format) noexcept;
        // Multiview view mask (must match the RenderingInfo used at draw time).
        GraphicsPipelineBuilder &set_view_mask(u32 mask) noexcept;

        // Replaces the default dynamic state set (viewport + scissor). Pass the full list you want.
        GraphicsPipelineBuilder &set_dynamic_states(span<const VkDynamicState> states);
        GraphicsPipelineBuilder &add_dynamic_state(VkDynamicState state);

        [[nodiscard]] RendererExpected<VulkanPipeline> create(VkDevice device,
                                                              VkPipelineCache cache = VK_NULL_HANDLE) const noexcept;

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
        ~VulkanPipelineCache();

        VulkanPipelineCache(const VulkanPipelineCache &) = delete;
        VulkanPipelineCache &operator=(const VulkanPipelineCache &) = delete;

        VulkanPipelineCache(VulkanPipelineCache &&o) noexcept;
        VulkanPipelineCache &operator=(VulkanPipelineCache &&o) noexcept;

        // Pass previously saved cache data to seed the cache; pass an empty span to start fresh.
        [[nodiscard]] static RendererExpected<VulkanPipelineCache> create(
            VkDevice device,
            span<const u8> initial_data = {}) noexcept;

        [[nodiscard]] VkPipelineCache vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        // Serializes the cache to a byte blob suitable for saving to disk and re-seeding next run.
        [[nodiscard]] RendererExpected<vector<u8>> serialize() const;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache cache_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
