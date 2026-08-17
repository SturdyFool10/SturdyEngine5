#include "TextAtlas.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

/// Returns the tile size for this `Renderer`.
///
/// @return Returns the current tile size value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 TextAtlas::tile_size() const noexcept { return max_tile_size_; }

/// Returns the current or globally available pixel range value.
///
/// @return Returns the current pixel range value.
/// @note This function does not throw exceptions.
[[nodiscard]] f32 TextAtlas::pixel_range() const noexcept { return config_.pixel_range; }

/// Formats atlas using the supplied arguments and current state.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] TextAtlas::FormatAtlas &TextAtlas::format_atlas(Text::RasterFormat format) noexcept {
            ZoneScopedN("TextAtlas::format_atlas");
            switch (format) {
                case Text::RasterFormat::SDF: return sdf_;
                case Text::RasterFormat::MSDF: return msdf_;
                case Text::RasterFormat::Color: return color_;
            }
            return sdf_;
        }

/// Formats atlas using the supplied arguments and current state.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const TextAtlas::FormatAtlas &TextAtlas::format_atlas(Text::RasterFormat format) const noexcept {
            ZoneScopedN("TextAtlas::format_atlas");
            return const_cast<TextAtlas *>(this)->format_atlas(format);
        }

/// Formats lru using the supplied arguments and current state.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] LruIndex<GlyphKey, GlyphKeyHash> &TextAtlas::format_lru(Text::RasterFormat format) noexcept {
            ZoneScopedN("TextAtlas::format_lru");
            switch (format) {
                case Text::RasterFormat::SDF: return sdf_lru_;
                case Text::RasterFormat::MSDF: return msdf_lru_;
                case Text::RasterFormat::Color: return color_lru_;
            }
            return sdf_lru_;
        }

/// Performs the texture format operation for `Renderer` using the supplied arguments.
///
/// @param format Format used for the resource, render target, or conversion.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::Format TextAtlas::texture_format(Text::RasterFormat format) const noexcept {
            ZoneScopedN("TextAtlas::texture_format");
            return format == Text::RasterFormat::SDF ? RHI::Format::R8Unorm : RHI::Format::RGBA8Unorm;
        }

} // namespace SFT::Renderer
