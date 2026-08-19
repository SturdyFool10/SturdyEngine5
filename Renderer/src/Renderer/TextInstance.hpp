#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cmath>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Renderer/Text/Text.hpp>
#include <Renderer/TextAtlas.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {


    struct GlyphInstance {
        glm::vec2 position{0.0f};
        glm::vec2 size{0.0f};
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};


        f32 rotation = 0.0f;


        f32 format_kind = 0.0f;


        f32 distance_pixel_range = 2.0f;


        f32 stem_darkening_px = 0.0f;
    };


    /// Formats kind value using the supplied arguments and current state.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr f32 format_kind_value(Text::RasterFormat format) noexcept {
        switch (format) {
            case Text::RasterFormat::SDF: return 0.0f;
            case Text::RasterFormat::MSDF: return 1.0f;
            case Text::RasterFormat::Color: return 2.0f;
        }
        return 0.0f;
    }


    struct GlyphPlacement {
        glm::vec2 position{0.0f};


        glm::vec2 size{0.0f};


        f32 rotation = 0.0f;
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 units_per_em = 1000;
        f32 pixel_size = 32.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        const Text::GlyphOutline *outline = nullptr;
        const Text::Font *font = nullptr;


        bool stem_darkening = true;
    };


    /// Resolves the requested value into the concrete value used by the engine.
    ///
    /// @param pixel_size Requested or available size for the operation.
    /// @param min_ppem `min_ppem` value used by the operation.
    /// @param max_ppem `max_ppem` value used by the operation.
    /// @param max_strength `max_strength` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr f32 resolved_stem_darkening_px(f32 pixel_size, f32 min_ppem = 14.0f, f32 max_ppem = 28.0f,
                                                            f32 max_strength = 0.22f) noexcept {
        if (pixel_size >= max_ppem) {
            return 0.0f;
        }
        if (pixel_size <= min_ppem) {
            return max_strength;
        }
        return max_strength * (1.0f - (pixel_size - min_ppem) / (max_ppem - min_ppem));
    }


    /// Creates a glyph instance value from the supplied arguments.
    ///
    /// @param position `position` value used by the operation.
    /// @param placement `placement` value used by the operation.
    /// @param slot Binding or storage slot addressed by the operation.
    /// @param atlas_pixel_range Range of values to process.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] GlyphInstance make_glyph_instance(glm::vec2 position, const GlyphPlacement &placement,
                                                            const GlyphSlot &slot, f32 atlas_pixel_range) noexcept;

    struct TextDrawBatch {
        Text::RasterFormat format = Text::RasterFormat::SDF;
        u32 tile_index = 0;


        RHI::Rect2D scissor{};


        u32 paint_group = 0;


        RHI::BufferHandle instance_buffer{};
        u32 first_instance = 0;
        u32 instance_count = 0;
        struct BoundGroup {
            u32 set = 0;
            RHI::BindGroupHandle handle{};
        };
        vector<BoundGroup> bind_groups;
    };


    struct TextFrameResources {
        struct BindingCacheEntry {
            Text::RasterFormat format = Text::RasterFormat::SDF;
            u32 tile_index = 0;
            RHI::TextureViewHandle atlas_view{};
            vector<TextDrawBatch::BoundGroup> bind_groups;
        };

        RHI::BufferHandle instance_buffer{};
        u64 instance_capacity_bytes = 0;


        vector<GlyphInstance> uploaded_instances;
        vector<BindingCacheEntry> binding_cache;
    };

    /// Destroys the text frame resources identified by the supplied parameters.
    ///
    /// @param device Device used or affected by the operation.
    /// @param resources `resources` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void destroy_text_frame_resources(RHI::RhiDevice &device, TextFrameResources &resources) noexcept;


    class TextPipeline {
      public:
        /// Constructs a `TextPipeline` in its default state.
        ///
        /// @note This function does not throw exceptions.
        TextPipeline() noexcept = default;

        /// Creates a `TextPipeline` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param color_format Format used for the resource, render target, or conversion.
        /// @param enable_shader_disk_cache Whether the associated behavior is enabled.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<TextPipeline> create(
            RHI::RhiDevice &device, RHI::Format color_format, bool enable_shader_disk_cache = true);


        /// Prepares the required state or resources for a later operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param atlas `atlas` value used by the operation.
        /// @param instances Instance used or affected by the operation.
        /// @param slots `slots` value used by the operation.
        /// @param instance_scissors Instance used or affected by the operation.
        /// @param instance_paint_groups Instance used or affected by the operation.
        /// @param resources `resources` value used by the operation.
        /// @param out_batches `out_batches` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult prepare(RHI::RhiDevice &device, const TextAtlas &atlas,
                                                   span<const GlyphInstance> instances, span<const GlyphSlot> slots,
                                                   span<const RHI::Rect2D> instance_scissors,
                                                   span<const u32> instance_paint_groups,
                                                   TextFrameResources &resources, vector<TextDrawBatch> &out_batches);

        /// Issues one instanced draw per batch against `pass`.
        ///
        /// @param pass Render-pass encoder that receives the draw commands.
        /// @param batches Batches prepare() built, drawn in order.
        /// @param viewport_size The render target's pixel dimensions.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Sets each batch's own scissor first — a caller does not need to (and should not)
        ///       call pass.set_scissor() itself around draw(), mirroring UiQuadPipeline::draw()'s
        ///       identical contract.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult draw(RHI::RenderPassEncoder &pass,
                                                span<const TextDrawBatch> batches, glm::vec2 viewport_size);

        /// Destroys or releases the `TextPipeline` resource represented by the supplied parameters.
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
        RHI::SamplerHandle sampler_{};
        ResourceBinding instances_binding_{};
        ResourceBinding texture_binding_{};
        ResourceBinding sampler_binding_{};
    };

} // namespace SFT::Renderer
