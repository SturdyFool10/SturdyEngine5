#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <clay.h>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Text/Text.hpp>

#include "Style.hpp"

using std::span;
using std::string;
using std::string_view;
using std::unordered_map;
using std::vector;

/// Package-internal (not re-exported from UI.hpp): bridges Clay's text-measurement callback and
/// UiRenderer's glyph drawing to the engine's existing CPU text-shaping stack
/// (Text::shape_line_with_fallback), reusing the exact approach
/// Renderer/RendererTextOverlay.cpp already uses rather than inventing a second one.
namespace SFT::UI {

    /// One resolved shape result, cached so the same (font, size, content) string is shaped once
    /// per frame rather than once per Clay_MeasureText call (Clay's own layout algorithm calls it
    /// many times for the same string while resolving FIT/GROW sizing) and reused again when
    /// UiRenderer turns the matching TEXT render command into glyph instances.
    struct CachedShape {
        Text::ShapedLine shaped;
        f32 width_px = 0.0f;
        f32 height_px = 0.0f;
        const Text::FontStack *fonts = nullptr;
    };

    class TextBridge {
      public:
        /// `font`/`emoji_fallback`/every font pointed to from `fallbacks` must outlive every
        /// subsequent measure/shape call that references `font_id` — same non-owning contract as
        /// Text::FontStack itself. `fallbacks` lets an application register any number of additional
        /// fonts (CJK, Arabic, a second symbol font, ...) tried in order for glyphs the primary font
        /// doesn't cover — each independently chosen by the application, not hardcoded to one
        /// category. Whichever font in the list actually has a given glyph wins (see
        /// Text::FontStack::fallbacks' own doc comment for the coverage-driven selection this feeds);
        /// order only matters when more than one listed font covers the same codepoint. Always
        /// non-color (an application wanting a color fallback uses `emoji_fallback` instead — the
        /// dedicated emoji face still wins for emoji-presentation runs regardless of `fallbacks`).
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr,
                           span<const Text::Font *const> fallbacks = {});

        [[nodiscard]] const Text::FontStack *font_stack(FontId font_id) const noexcept;

        /// Clears the per-frame shape cache. Call once per begin_layout().
        void begin_frame() noexcept;

        /// Shapes (or returns the cached result for) `content` at `style`'s font/size/spacing.
        /// Returns nullptr if `style.font_id` was never registered or shaping failed.
        [[nodiscard]] const CachedShape *shape_and_cache(const TextStyle &style, string_view utf8_content);

        /// Matches Clay_SetMeasureTextFunction's required signature exactly; `user_data` must be the
        /// `TextBridge*` this was registered with.
        [[nodiscard]] static Clay_Dimensions measure_callback(Clay_StringSlice text, Clay_TextElementConfig *config,
                                                               void *user_data);

      private:
        [[nodiscard]] Clay_Dimensions measure(Clay_StringSlice text, const Clay_TextElementConfig &config);

        struct FontEntry {
            FontId id = 0;
            Text::FontStack stack{};
            /// Backing storage for stack.fallbacks (a non-owning span) — moving a FontEntry (e.g.
            /// fonts_ reallocating on push_back) moves this vector's heap buffer by pointer, not by
            /// element copy, so the span stays valid; only re-registering the same font_id needs the
            /// span recomputed (see register_font()'s own two call sites for why).
            vector<Text::FallbackFont> owned_fallbacks;
        };

        struct ShapeCacheKey {
            FontId font_id = 0;
            u16 font_size = 0;
            u16 letter_spacing = 0;
            string content;
            [[nodiscard]] friend bool operator==(const ShapeCacheKey &, const ShapeCacheKey &) = default;
        };

        struct ShapeCacheKeyHash {
            [[nodiscard]] usize operator()(const ShapeCacheKey &key) const noexcept;
        };

        [[nodiscard]] const FontEntry *find_font(FontId id) const noexcept;

        vector<FontEntry> fonts_;
        unordered_map<ShapeCacheKey, CachedShape, ShapeCacheKeyHash> cache_;
    };

} // namespace SFT::UI
