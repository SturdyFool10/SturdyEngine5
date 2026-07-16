#include "TextAtlas.hpp"

namespace SFT::Renderer {

[[nodiscard]] u32 TextAtlas::tile_size() const noexcept { return max_tile_size_; }

[[nodiscard]] f32 TextAtlas::pixel_range() const noexcept { return config_.pixel_range; }

[[nodiscard]] TextAtlas::FormatAtlas &TextAtlas::format_atlas(Text::RasterFormat format) noexcept {
            switch (format) {
                case Text::RasterFormat::SDF: return sdf_;
                case Text::RasterFormat::MSDF: return msdf_;
                case Text::RasterFormat::Color: return color_;
            }
            return sdf_;
        }

[[nodiscard]] const TextAtlas::FormatAtlas &TextAtlas::format_atlas(Text::RasterFormat format) const noexcept {
            return const_cast<TextAtlas *>(this)->format_atlas(format);
        }

[[nodiscard]] LruIndex<GlyphKey, GlyphKeyHash> &TextAtlas::format_lru(Text::RasterFormat format) noexcept {
            switch (format) {
                case Text::RasterFormat::SDF: return sdf_lru_;
                case Text::RasterFormat::MSDF: return msdf_lru_;
                case Text::RasterFormat::Color: return color_lru_;
            }
            return sdf_lru_;
        }

[[nodiscard]] RHI::Format TextAtlas::texture_format(Text::RasterFormat format) const noexcept {
            return format == Text::RasterFormat::SDF ? RHI::Format::R8Unorm : RHI::Format::RGBA8Unorm;
        }

} // namespace SFT::Renderer
