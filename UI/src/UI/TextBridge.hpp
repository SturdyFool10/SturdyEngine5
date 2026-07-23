#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <clay.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Text/Text.hpp>

#include "Style.hpp"

using std::string;
using std::string_view;
using std::unordered_map;
using std::vector;

// Package-internal (not re-exported from UI.hpp): bridges Clay's text-measurement callback and
// UiRenderer's glyph drawing to the engine's existing CPU text-shaping stack
// (Text::shape_line_with_fallback), reusing the exact approach
// Renderer/RendererTextOverlay.cpp already uses rather than inventing a second one.
namespace SFT::UI {

    // One resolved shape result, cached so the same (font, size, content) string is shaped once
    // per frame rather than once per Clay_MeasureText call (Clay's own layout algorithm calls it
    // many times for the same string while resolving FIT/GROW sizing) and reused again when
    // UiRenderer turns the matching TEXT render command into glyph instances.
    struct CachedShape {
        Text::ShapedLine shaped;
        f32 width_px = 0.0f;
        f32 height_px = 0.0f;
        const Text::FontStack *fonts = nullptr;
    };

    class TextBridge {
      public:
        // `font`/`emoji_fallback` must outlive every subsequent measure/shape call that references
        // `font_id` — same non-owning contract as Text::FontStack itself.
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr);

        [[nodiscard]] const Text::FontStack *font_stack(FontId font_id) const noexcept;

        // Clears the per-frame shape cache. Call once per begin_layout().
        void begin_frame() noexcept;

        // Shapes (or returns the cached result for) `content` at `style`'s font/size/spacing.
        // Returns nullptr if `style.font_id` was never registered or shaping failed.
        [[nodiscard]] const CachedShape *shape_and_cache(const TextStyle &style, string_view utf8_content);

        // Matches Clay_SetMeasureTextFunction's required signature exactly; `user_data` must be the
        // `TextBridge*` this was registered with.
        [[nodiscard]] static Clay_Dimensions measure_callback(Clay_StringSlice text, Clay_TextElementConfig *config,
                                                               void *user_data);

      private:
        [[nodiscard]] Clay_Dimensions measure(Clay_StringSlice text, const Clay_TextElementConfig &config);

        struct FontEntry {
            FontId id = 0;
            Text::FontStack stack{};
        };

        struct ShapeCacheKey {
            FontId font_id = 0;
            u16 font_size = 0;
            u16 letter_spacing = 0;
            string content;
            [[nodiscard]] friend bool operator==(const ShapeCacheKey &, const ShapeCacheKey &) = default;
        };

        struct ShapeCacheKeyHash {
            [[nodiscard]] usize operator()(const ShapeCacheKey &key) const noexcept {
                usize seed = std::hash<string>{}(key.content);
                seed ^= (static_cast<usize>(key.font_id) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                seed ^= (static_cast<usize>(key.font_size) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                seed ^= (static_cast<usize>(key.letter_spacing) << 1) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        [[nodiscard]] const FontEntry *find_font(FontId id) const noexcept;

        vector<FontEntry> fonts_;
        unordered_map<ShapeCacheKey, CachedShape, ShapeCacheKeyHash> cache_;
    };

} // namespace SFT::UI
