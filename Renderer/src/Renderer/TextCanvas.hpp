#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/Core.hpp>
#include <Text/Text.hpp>
#include "TileGrid.hpp"
#include "TextAtlas.hpp"
#include "TextInstance.hpp"

using std::span;
using std::unordered_map;
using std::vector;

namespace SFT::Renderer {


    struct ResidentCanvasTile {
        TileCoord coord{};
        RHI::TextureViewHandle view{};
        RHI::Rect2D logical_rect{};
    };


    class TextCanvas {
      public:
        struct Config {
            u32 desired_tile_size = 2048;
            u32 max_resident_tiles = 64;
        };

        /// Constructs a `TextCanvas` in its default state.
        ///
        /// @note This function does not throw exceptions.
        TextCanvas() noexcept = default;


        /// Creates a `TextCanvas` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param config Configuration values controlling the operation.
        /// @param atlas `atlas` value used by the operation.
        /// @param pipeline Pipeline used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] static Core::RendererExpected<TextCanvas> create(RHI::RhiDevice &device, const Config &config,
                                                                       TextAtlas &atlas, TextPipeline &pipeline);


        /// Draws run using the current rendering state.
        ///
        /// @param glyphs `glyphs` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void draw_run(span<const GlyphPlacement> glyphs);


        /// Finds or creates the viewport resident required by the operation.
        ///
        /// @param device Device used or affected by the operation.
        /// @param viewport `viewport` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<vector<ResidentCanvasTile>> ensure_viewport_resident(RHI::RhiDevice &device,
                                                                                                   RHI::Rect2D viewport);

        /// Returns the tile size for this `TextCanvas`.
        ///
        /// @return Returns the current tile size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 tile_size() const noexcept;

        /// Destroys or releases the `TextCanvas` resource represented by the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy(RHI::RhiDevice &device) noexcept;

      private:
        struct TileRecord {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::TextureLayout current_layout = RHI::TextureLayout::Undefined;
            TextFrameResources text_resources{};
            bool dirty = true;
        };

        /// Renders tile using the current rendering state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param coord `coord` value used by the operation.
        /// @param tile `tile` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult render_tile(RHI::RhiDevice &device, TileCoord coord, TileRecord &tile);
        /// Performs the evict if over budget operation for `TextCanvas` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        /// @param keep `keep` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererResult evict_if_over_budget(RHI::RhiDevice &device, span<const TileCoord> keep);

        Config config_{};
        u32 tile_size_ = 0;
        TextAtlas *atlas_ = nullptr;
        TextPipeline *pipeline_ = nullptr;
        unordered_map<TileCoord, TileRecord, TileCoordHash> resident_tiles_;
        unordered_map<TileCoord, vector<GlyphPlacement>, TileCoordHash> tile_glyphs_;
        LruIndex<TileCoord, TileCoordHash> lru_;
    };

} // namespace SFT::Renderer
