#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <atomic>
#include <glm/vec2.hpp>
#include <memory>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <RHI/Threading.hpp>
#include <Renderer/RendererModule.hpp>
#include <Renderer/TextAtlas.hpp>
#include <Renderer/TextInstance.hpp>

#include "Context.hpp"
#include "UiCustomElementPipeline.hpp"
#include "UiQuadPipeline.hpp"

using std::vector;

namespace SFT::UI {


    class UiRenderer {
      public:
        /// Constructs a `UiRenderer` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UiRenderer() noexcept = default;
        /// Constructs a `UiRenderer` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        UiRenderer(UiRenderer &&other) noexcept;
        /// Assigns a new value to this `UiRenderer`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        UiRenderer &operator=(UiRenderer &&other) noexcept;
        /// Disables this construction form for `UiRenderer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        UiRenderer(const UiRenderer &) = delete;
        /// Assigns a new value to this `UiRenderer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        UiRenderer &operator=(const UiRenderer &) = delete;

        /// Creates a `UiRenderer` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<UiRenderer> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param snapshot `snapshot` value used by the operation.
        /// @param texture_resolver Texture used or affected by the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame_resource_index Zero-based index of the target element or entry.
        /// @param out_transient_buffers Buffer used or affected by the operation.
        /// @param out_retired_atlas_resources `out_retired_atlas_resources` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                    const FrameSnapshot &snapshot, Renderer::Renderer *texture_resolver,
                                                    Core::RenderSurfaceHandle surface, u32 frame_resource_index,
                                                    vector<RHI::BufferHandle> &out_transient_buffers,
                                                    Renderer::TextAtlasRetiredResources &out_retired_atlas_resources);

        /// Issues the batches prepare() built, interleaved by paint order (see class doc comment).
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param viewport_size Requested or available size for the operation.
        /// @param surface Surface used or affected by the operation.
        /// @param frame_resource_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Each pipeline sets its own scissor per batch; the caller should not rely on scissor
        ///       (or bound-pipeline) state surviving this call.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, glm::vec2 viewport_size,
                                                 Core::RenderSurfaceHandle surface, u32 frame_resource_index);

        /// Destroys or releases the `UiRenderer` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;
        /// Reads the requested data from the associated source.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool ready() const noexcept;


        /// Returns the current or globally available generation value.
        ///
        /// @return Returns the current generation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 generation() const noexcept;

      private:
        Renderer::TextPipeline text_pipeline_;
        UiQuadPipeline quad_pipeline_;
        UiCustomElementPipeline custom_element_pipeline_;
        struct FrameResources {
            Renderer::TextFrameResources text;
            UiQuadFrameResources quads;
            vector<Renderer::TextDrawBatch> text_batches;
            vector<UiQuadDrawBatch> quad_batches;


            vector<CustomDraw> custom_draws;
            vector<u32> custom_group_ids;
        };
        struct SurfaceFrameResources {
            Core::RenderSurfaceHandle surface{};
            Renderer::TextAtlas text_atlas;
            vector<FrameResources> frames;
        };
        vector<SurfaceFrameResources> surface_frame_resources_;


        RHI::Format color_format_{};
        std::atomic<u64> generation_{0};


        bool enable_shader_disk_cache_ = true;


        Renderer::TextureHandle white_texture_{};


        std::shared_ptr<Async::Mutex<u8>> operation_mutex_ = std::make_shared<Async::Mutex<u8>>(0);
        bool ready_ = false;
    };

} // namespace SFT::UI
