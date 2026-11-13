#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <array>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <Renderer/UI/UiQuad.hpp>

using std::span;
using std::vector;

namespace SFT::UI {


    /// Bind groups a quad batch needs set before it draws: the shader's resources live in a few descriptor sets (one in
    /// practice, plus the one an emulated push-constant block takes on WebGPU). Kept inline in the batch so building a
    /// frame's batches allocates nothing.
    inline constexpr usize kUiQuadMaxBindGroups = 4;

    struct UiQuadDrawBatch {
        /// The texture the whole batch samples. Only meaningful when textures are bound one batch at a time; when they
        /// are bound as an array (see `UiQuadPipeline::bindless()`) the batch holds many textures and this is empty.
        RHI::TextureViewHandle texture_view{};
        /// `TextureResource::revision` of the texture the view belongs to (0 for a view that is not a renderer texture).
        u64 texture_revision = 0;
        RHI::Rect2D scissor{};
        RHI::BufferHandle instance_buffer{};
        u32 first_instance = 0;
        u32 instance_count = 0;


        u32 paint_group = 0;
        struct BoundGroup {
            u32 set = 0;
            RHI::BindGroupHandle handle{};
        };
        std::array<BoundGroup, kUiQuadMaxBindGroups> bind_groups{};
        u32 bind_group_count = 0;

        /// `true` draws with additive (One,One) GPU blending instead of the normal alpha blend — for
        /// adding light on top of already-drawn content (a bloom composite) rather than compositing a
        /// normal opaque/translucent UI element. A caller sets this on the batch prepare() returned,
        /// not via any per-instance field, since blend state is a pipeline-level property.
        bool additive = false;
    };


    /// What the quad pipeline did while preparing one frame, for the engine's UI statistics.
    struct UiQuadStats {
        u32 instances = 0;
        u32 batches = 0;
        /// Textures the frame's quads sample (the size of the bindless table, or the number of distinct bind groups).
        u32 textures = 0;
        /// Bind groups created this frame (a new texture, or a changed texture table).
        u32 bind_groups_created = 0;
        /// Bytes written to the instance buffer (0 when the frame's instances matched the last upload).
        u64 bytes_uploaded = 0;
    };

    /// What the last quad draw left bound on a render pass, so the next one can skip setting it again. The caller owns
    /// it for as long as nothing else has drawn on the pass since (any other pipeline's draw disturbs it: reset it).
    struct UiQuadDrawState {
        RHI::RenderPipelineHandle pipeline{};
        std::array<UiQuadDrawBatch::BoundGroup, kUiQuadMaxBindGroups> bind_groups{};
        u32 bind_group_count = 0;
        bool scissor_set = false;
        RHI::Rect2D scissor{};
    };

    struct UiQuadFrameResources {
        struct BindingCacheEntry {


            RHI::TextureViewHandle texture_view{};
            u64 texture_revision = 0;
            /// Marks the entries the frame being prepared draws; the others are destroyed.
            bool used = false;
            std::array<UiQuadDrawBatch::BoundGroup, kUiQuadMaxBindGroups> bind_groups{};
            u32 bind_group_count = 0;
        };

        /// One texture of the bindless table, identified the way the per-batch cache identifies one.
        struct TableEntry {
            RHI::TextureViewHandle view{};
            u64 revision = 0;
        };

        RHI::BufferHandle instance_buffer{};
        u64 instance_capacity_bytes = 0;
        /// What the instance buffer holds, to skip a write when the next frame's records are the same.
        vector<UiQuadInstanceGpu> uploaded_instances;
        /// Where the next frame's records are built (swapped with `uploaded_instances` after an upload, so neither
        /// vector is reallocated frame after frame).
        vector<UiQuadInstanceGpu> staged_instances;
        vector<BindingCacheEntry> binding_cache;

        // Bindless: the textures the frame draws, in the order their indices were handed out, and the one bind group
        // (instances, samplers and this table) every batch of the frame uses. The table only changes when the set of
        // textures does, so a screen that keeps drawing the same pictures keeps the same bind group.
        vector<TableEntry> table;
        std::unordered_map<u64, u32> table_index;
        vector<TableEntry> table_wanted;
        vector<u8> table_used;
        std::array<UiQuadDrawBatch::BoundGroup, kUiQuadMaxBindGroups> table_groups{};
        u32 table_group_count = 0;

        UiQuadStats stats{};
    };

    /// Destroys the UI quad frame resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_ui_quad_frame_resources(RHI::RhiDevice &device, UiQuadFrameResources &resources) noexcept;


    class UiQuadPipeline {
      public:
        /// Constructs a `UiQuadPipeline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        UiQuadPipeline() noexcept = default;

        /// Creates a `UiQuadPipeline` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<UiQuadPipeline> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param instances Instance used or affected by the operation.
        /// @param instance_texture_views Texture used or affected by the operation.
        /// @param instance_texture_revisions One per instance: the revision of the texture its view belongs to. A cached
        ///        bind group is only reused when both the view and the revision match, since a destroyed view's handle
        ///        value can be handed out again for a different texture.
        /// @param instance_scissors Instance used or affected by the operation.
        /// @param instance_paint_groups Instance used or affected by the operation.
        /// @param resources `resources` value used by the operation.
        /// @param out_batches `out_batches` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, span<const UiQuadInstance> instances,
                                                    span<const RHI::TextureViewHandle> instance_texture_views,
                                                    span<const u64> instance_texture_revisions,
                                                    span<const RHI::Rect2D> instance_scissors,
                                                    span<const u32> instance_paint_groups,
                                                    UiQuadFrameResources &resources, vector<UiQuadDrawBatch> &out_batches);

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
        /// @param state When given, what an earlier call left bound is not set again, and this call updates it.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass, span<const UiQuadDrawBatch> batches,
                                                 glm::vec2 viewport_size, UiQuadDrawState *state = nullptr);

        /// Whether this pipeline binds the frame's textures as one array, so batches no longer split on texture. True
        /// when the device supports descriptor indexing (Vulkan and D3D12 desktop GPUs); the one-bind-group-per-texture
        /// path remains for the rest (WebGPU, old drivers).
        [[nodiscard]] bool bindless() const noexcept { return bindless_; }

        /// Destroys or releases the `UiQuadPipeline` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        /// Textures one frame's bindless table can hold at most, and the least a device has to offer to use one.
        static constexpr u32 kBindlessCapacity = 2048;
        static constexpr u32 kMinBindlessCapacity = 256;

        [[nodiscard]] static Core::RendererExpected<UiQuadPipeline> create_variant(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache, u32 bindless_capacity);

        struct ResourceBinding {
            usize layout_index = 0;
            u32 binding = 0;
            bool found = false;
        };

        RHI::ShaderModuleHandle vertex_module_{};
        RHI::ShaderModuleHandle fragment_module_{};
        RHI::PipelineLayoutHandle pipeline_layout_{};
        RHI::RenderPipelineHandle pipeline_{};
        RHI::RenderPipelineHandle additive_pipeline_{};
        vector<RHI::BindGroupLayoutHandle> bind_group_layouts_;
        vector<u32> bind_group_layout_sets_;
        RHI::SamplerHandle sampler_{};
        /// Point-filtered sampler for `UiQuadKind::ImageNearest` quads (pixel art).
        RHI::SamplerHandle nearest_sampler_{};
        /// Whether the shader takes its textures as one array (`SFT_UI_BINDLESS`) instead of one at a time.
        bool bindless_ = false;
        /// Most textures one frame's table can hold when `bindless_`.
        u32 bindless_capacity_ = 0;
        ResourceBinding instances_binding_{};
        ResourceBinding texture_binding_{};
        ResourceBinding sampler_binding_{};
        ResourceBinding nearest_sampler_binding_{};
    };

} // namespace SFT::UI
