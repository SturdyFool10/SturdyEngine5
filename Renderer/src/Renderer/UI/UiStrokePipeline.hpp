#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <Renderer/UI/UiStroke.hpp>

using std::span;
using std::vector;

namespace SFT::UI {


    struct UiStrokeDrawBatch {
        RHI::Rect2D scissor{};
        RHI::BufferHandle instance_buffer{};
        u32 first_instance = 0;
        u32 instance_count = 0;


        u32 paint_group = 0;
        struct BoundGroup {
            u32 set = 0;
            RHI::BindGroupHandle handle{};
        };
        vector<BoundGroup> bind_groups;
    };


    struct UiStrokeFrameResources {
        RHI::BufferHandle instance_buffer{};
        u64 instance_capacity_bytes = 0;
        vector<UiStrokeInstance> uploaded_instances;


        vector<UiStrokeDrawBatch::BoundGroup> bind_groups;
        bool bind_groups_valid = false;
    };

    /// Destroys the UI stroke frame resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_ui_stroke_frame_resources(RHI::RhiDevice &device, UiStrokeFrameResources &resources) noexcept;


    // Untextured sibling of UiQuadPipeline (UiQuadPipeline.hpp) — draws polyline segments as
    // antialiased capsules instead of rounded rects. See Shaders/ui_stroke.slang's own doc comment for
    // the instancing/AA model.
    class UiStrokePipeline {
      public:
        /// Constructs a `UiStrokePipeline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UiStrokePipeline() noexcept = default;

        /// Creates a `UiStrokePipeline` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<UiStrokePipeline> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param instances Instance used or affected by the operation.
        /// @param instance_scissors Instance used or affected by the operation.
        /// @param instance_paint_groups Instance used or affected by the operation.
        /// @param resources `resources` value used by the operation.
        /// @param out_batches `out_batches` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, span<const UiStrokeInstance> instances,
                                                    span<const RHI::Rect2D> instance_scissors,
                                                    span<const u32> instance_paint_groups,
                                                    UiStrokeFrameResources &resources, vector<UiStrokeDrawBatch> &out_batches);

        /// Draws the requested content using the current rendering state.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param batches Batches prepare() built, drawn in order.
        /// @param viewport_size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Sets each batch's own scissor rect before drawing it — a caller does not need to
        ///       (and should not) call pass.set_scissor() itself around draw().
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, span<const UiStrokeDrawBatch> batches,
                                                 glm::vec2 viewport_size);

        /// Destroys or releases the `UiStrokePipeline` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct ResourceBinding {
            usize layout_index = 0;
            u32 binding = 0;
            bool found = false;
        };

        RHI::ShaderModuleHandle vertex_module_{};
        RHI::ShaderModuleHandle fragment_module_{};
        RHI::PipelineLayoutHandle pipeline_layout_{};
        RHI::RenderPipelineHandle pipeline_{};
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts_;
        vector<u32> bind_group_layout_sets_;
        ResourceBinding instances_binding_{};
    };

} // namespace SFT::UI
