#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cstring>
#pragma endregion

#include "TextBridge.hpp"

namespace SFT::UI {

    void TextBridge::register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback) {
        for (FontEntry &entry : fonts_) {
            if (entry.id == font_id) {
                entry.stack = Text::FontStack{
                    .primary = &font,
                    .emoji = emoji_fallback,
                    .primary_font_id = static_cast<u64>(font_id),
                    .emoji_font_id = static_cast<u64>(font_id) | (u64{1} << 32),
                    .emoji_is_color = true,
                };
                return;
            }
        }
        fonts_.push_back(FontEntry{
            .id = font_id,
            .stack = Text::FontStack{
                .primary = &font,
                .emoji = emoji_fallback,
                .primary_font_id = static_cast<u64>(font_id),
                .emoji_font_id = static_cast<u64>(font_id) | (u64{1} << 32),
                .emoji_is_color = true,
            },
        });
    }

    const TextBridge::FontEntry *TextBridge::find_font(FontId id) const noexcept {
        for (const FontEntry &entry : fonts_) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    const Text::FontStack *TextBridge::font_stack(FontId font_id) const noexcept {
        const FontEntry *entry = find_font(font_id);
        return entry ? &entry->stack : nullptr;
    }

    void TextBridge::begin_frame() noexcept { cache_.clear(); }

    const CachedShape *TextBridge::shape_and_cache(const TextStyle &style, string_view utf8_content) {
        const FontEntry *entry = find_font(style.font_id);
        if (entry == nullptr || entry->stack.primary == nullptr) {
            return nullptr;
        }

        ShapeCacheKey key{
            .font_id = style.font_id,
            .font_size = style.font_size,
            .letter_spacing = style.letter_spacing,
            .content = string{utf8_content},
        };
        if (auto cached = cache_.find(key); cached != cache_.end()) {
            return &cached->second;
        }

        const UString content{utf8_content.data(), utf8_content.size()};
        auto shaped = Text::shape_line_with_fallback(entry->stack, content.as_ustr());
        if (!shaped) {
            return nullptr;
        }

        const Text::Font &font = *entry->stack.primary;
        const u32 units_per_em = font.units_per_em();
        const f32 scale = units_per_em > 0 ? static_cast<f32>(style.font_size) / static_cast<f32>(units_per_em) : 0.0f;
        const f32 font_line_height =
            static_cast<f32>(font.ascender() - font.descender() + font.line_gap()) * scale;
        const f32 height_px = style.line_height != 0 ? static_cast<f32>(style.line_height)
                                                       : std::max(font_line_height, static_cast<f32>(style.font_size));

        CachedShape entry_value{
            .shaped = std::move(*shaped),
            .width_px = shaped->advance_em * static_cast<f32>(style.font_size),
            .height_px = height_px,
            .fonts = &entry->stack,
        };
        auto [inserted, _] = cache_.emplace(std::move(key), std::move(entry_value));
        return &inserted->second;
    }

    Clay_Dimensions TextBridge::measure(Clay_StringSlice text, const Clay_TextElementConfig &config) {
        TextStyle style{
            .font_id = config.fontId,
            .font_size = config.fontSize,
            .letter_spacing = config.letterSpacing,
            .line_height = config.lineHeight,
        };
        const CachedShape *shape = shape_and_cache(style, string_view{text.chars, static_cast<usize>(text.length)});
        if (shape == nullptr) {
            return Clay_Dimensions{.width = 0.0f, .height = static_cast<f32>(config.fontSize)};
        }
        return Clay_Dimensions{.width = shape->width_px, .height = shape->height_px};
    }

    Clay_Dimensions TextBridge::measure_callback(Clay_StringSlice text, Clay_TextElementConfig *config, void *user_data) {
        if (config == nullptr || user_data == nullptr) {
            return Clay_Dimensions{};
        }
        return static_cast<TextBridge *>(user_data)->measure(text, *config);
    }

} // namespace SFT::UI
