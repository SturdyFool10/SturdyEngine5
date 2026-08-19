#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Renderer/Text/Text.hpp>
#include <Renderer/TileGrid.hpp>

using std::span;
using std::unordered_map;
using std::vector;

namespace SFT::Renderer {


    struct GlyphKey {
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 reference_ppem = 0;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(const GlyphKey &, const GlyphKey &) noexcept = default;
    };

    struct GlyphKeyHash {
        /// Invokes the callable behavior provided by `GlyphKeyHash`.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize operator()(const GlyphKey &key) const noexcept;
    };


    struct GlyphRequest {
        u64 font_id = 0;
        u32 glyph_id = 0;
        u32 units_per_em = 1000;
        f32 pixel_size = 32.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;
        const Text::GlyphOutline *outline = nullptr;
        const Text::Font *font = nullptr;
    };


    struct TextAtlasRetiredResources {
        vector<RHI::TextureHandle> textures;
        vector<RHI::TextureViewHandle> texture_views;
    };


    struct GlyphSlot {
        u32 tile_index = 0;
        glm::vec2 uv_min{0.0f};
        glm::vec2 uv_max{0.0f};
        glm::vec2 raster_size_px{0.0f};


        f32 reference_ppem = 0.0f;
        Text::RasterFormat format = Text::RasterFormat::SDF;


        f32 bearing_x = 0.0f;
        f32 bearing_top = 0.0f;
    };


    class TextAtlas {
      public:
        struct Config {
            u32 initial_image_size = 64;
            u32 maximum_image_size = 4096;
            f32 pixel_range = 4.0f;
            f32 padding_px = 4.0f;
        };

        /// Constructs a `TextAtlas` in its default state.
        ///
        /// @note This function does not throw exceptions.
        TextAtlas() noexcept = default;

        /// Creates a `TextAtlas` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<TextAtlas> create(RHI::RhiDevice &device, const Config &config);


        /// Finds or creates the resident required by the operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param requests `requests` value used by the operation.
        /// @param out_slots `out_slots` value used by the operation.
        /// @param out_transient_buffers Buffer used or affected by the operation.
        /// @param out_retired_resources `out_retired_resources` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult ensure_resident(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                           span<const GlyphRequest> requests, vector<GlyphSlot> &out_slots,
                                                           vector<RHI::BufferHandle> &out_transient_buffers,
                                                           TextAtlasRetiredResources &out_retired_resources);

        /// Performs the tile view operation for `TextAtlas` using the supplied arguments.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param tile_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::TextureViewHandle tile_view(Text::RasterFormat format, u32 tile_index) const noexcept;
        /// Returns the tile count for this `TextAtlas`.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 tile_count(Text::RasterFormat format) const noexcept;
        /// Returns the tile size for this `TextAtlas`.
        ///
        /// @return Returns the current tile size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 tile_size() const noexcept;
        /// Returns the current or globally available pixel range value.
        ///
        /// @return Returns the current pixel range value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 pixel_range() const noexcept;

        /// Destroys or releases the `TextAtlas` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct AtlasRect {
            u32 x = 0;
            u32 y = 0;
            u32 width = 0;
            u32 height = 0;
        };

        struct Tile {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
            u32 size = 0;


            vector<AtlasRect> free_rects;
        };

        struct FormatAtlas {
            vector<Tile> tiles;
        };

        struct RectLocation {
            u32 tile_index = 0;
            u32 x = 0;
            u32 y = 0;
            u32 raster_width = 0;
            u32 raster_height = 0;
            f32 reference_ppem = 0.0f;


            f32 bearing_x = 0.0f;
            f32 bearing_top = 0.0f;
        };

        struct PendingUpload {
            usize request_index = 0;
            GlyphKey key{};
            RectLocation rect{};
            bool allocated = false;
        };

        /// Allocates rect.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param protected_keys `protected_keys` value used by the operation.
        /// @param out_retired_resources `out_retired_resources` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<RectLocation>
        allocate_rect(RHI::RhiDevice &device, RHI::CommandEncoder &encoder, Text::RasterFormat format,
                      u32 width, u32 height, span<const GlyphKey> protected_keys,
                      TextAtlasRetiredResources &out_retired_resources);
        /// Creates a tile from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<Tile> create_tile(RHI::RhiDevice &device,
                                                               Text::RasterFormat format, u32 size);
        /// Appends the supplied value or range to the current contents.
        ///
        /// @param device Device used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult append_tile(RHI::RhiDevice &device, Text::RasterFormat format, u32 size);
        /// Grows tile using the supplied arguments and current state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param new_size Requested or available size for the operation.
        /// @param out_retired_resources `out_retired_resources` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult grow_tile(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                     Text::RasterFormat format, u32 new_size,
                                                     TextAtlasRetiredResources &out_retired_resources);
        /// Releases rect using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param rect `rect` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void release_rect(Text::RasterFormat format, RectLocation rect) noexcept;
        /// Uploads misses using the supplied arguments and current state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param encoder `encoder` value used by the operation.
        /// @param requests `requests` value used by the operation.
        /// @param misses `misses` value used by the operation.
        /// @param out_slots `out_slots` value used by the operation.
        /// @param out_transient_buffers Buffer used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult upload_misses(RHI::RhiDevice &device, RHI::CommandEncoder &encoder,
                                                         span<const GlyphRequest> requests, const vector<PendingUpload> &misses,
                                                         vector<GlyphSlot> &out_slots, vector<RHI::BufferHandle> &out_transient_buffers);

        /// Formats atlas using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] FormatAtlas &format_atlas(Text::RasterFormat format) noexcept;
        /// Formats atlas using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FormatAtlas &format_atlas(Text::RasterFormat format) const noexcept;
        /// Formats lru using the supplied arguments and current state.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] LruIndex<GlyphKey, GlyphKeyHash> &format_lru(Text::RasterFormat format) noexcept;
        /// Performs the texture format operation for `TextAtlas` using the supplied arguments.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::Format texture_format(Text::RasterFormat format) const noexcept;
        /// Performs the slot from rect operation for `TextAtlas` using the supplied arguments.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param rect `rect` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] GlyphSlot slot_from_rect(Text::RasterFormat format, RectLocation rect) const noexcept;

        Config config_{};
        u32 initial_tile_size_ = 0;
        u32 max_tile_size_ = 0;
        FormatAtlas sdf_;
        FormatAtlas msdf_;
        FormatAtlas color_;
        unordered_map<GlyphKey, RectLocation, GlyphKeyHash> resident_;
        LruIndex<GlyphKey, GlyphKeyHash> sdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> msdf_lru_;
        LruIndex<GlyphKey, GlyphKeyHash> color_lru_;
    };

} // namespace SFT::Renderer
