#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <cstring>
#pragma endregion

#include <Renderer/Text/EmbeddedFont.hpp>
#include <Renderer/UI/TextBridge.hpp>

namespace SFT::UI {

    /// Registers font using the supplied arguments and current state.
    ///
    /// @param font_id Identifier of the target object or resource.
    /// @param font `font` value used by the operation.
    /// @param emoji_fallback Fallback value used when the primary value is unavailable.
    /// @param fallbacks Fallback value used when the primary value is unavailable.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextBridge::register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback,
                                   span<const Text::Font *const> fallbacks) {
        // 64-bit cache font id: the UI font id, the slot within its stack (0 primary, 1 emoji, 2+ fallbacks) at bit 32,
        // and the registration generation at bit 48.
        const auto cache_id = [font_id](u16 generation, u64 slot) {
            return static_cast<u64>(font_id) | (slot << 32) | (u64{generation} << 48);
        };
        const auto build = [&](FontEntry &entry) {
            entry.owned_fallbacks.clear();
            entry.owned_fallbacks.reserve(fallbacks.size());
            for (usize i = 0; i < fallbacks.size(); ++i) {
                entry.owned_fallbacks.push_back(Text::FallbackFont{
                    .font = fallbacks[i],
                    .font_id = cache_id(entry.generation, u64{2 + i}),
                    .is_color = false,
                });
            }
            entry.stack = Text::FontStack{
                .primary = &font,
                .emoji = emoji_fallback,
                .primary_font_id = cache_id(entry.generation, 0),
                .emoji_font_id = cache_id(entry.generation, 1),
                .fallbacks = entry.owned_fallbacks,
                .emoji_is_color = true,
            };
        };

        for (FontEntry &entry : fonts_) {
            if (entry.id != font_id) {
                continue;
            }
            // Re-registering the same fonts (the per-frame replay) keeps every cache warm; a different font behind the
            // id (a live font change) starts a new generation and drops the shapes measured with the old one.
            bool same = entry.stack.primary == &font && entry.stack.emoji == emoji_fallback &&
                        entry.owned_fallbacks.size() == fallbacks.size();
            for (usize i = 0; same && i < fallbacks.size(); ++i) {
                same = entry.owned_fallbacks[i].font == fallbacks[i];
            }
            if (same) {
                return;
            }
            ++entry.generation;
            std::erase_if(cache_, [font_id](const auto &cached) { return cached.first.font_id == font_id; });
            build(entry);
            return;
        }

        FontEntry new_entry{.id = font_id};
        build(new_entry);
        fonts_.push_back(std::move(new_entry));
    }

    void TextBridge::set_font_features(FontId font_id, string_view comma_separated_features) {
        for (FontEntry &entry : fonts_) {
            if (entry.id == font_id && entry.features != comma_separated_features) {
                entry.features = string{comma_separated_features};
                std::erase_if(cache_, [font_id](const auto &cached) { return cached.first.font_id == font_id; });
            }
        }
    }

    void TextBridge::unregister_font(FontId font_id) {
        std::erase_if(cache_, [font_id](const auto &cached) { return cached.first.font_id == font_id; });
        std::erase_if(fonts_, [font_id](const FontEntry &entry) { return entry.id == font_id; });
    }

    /// Finds font in the available state.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const TextBridge::FontEntry *TextBridge::find_font(FontId id) const noexcept {
        for (const FontEntry &entry : fonts_) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    const TextBridge::FontEntry *TextBridge::ensure_default_font() {
        if (!default_font_attempted_) {
            default_font_attempted_ = true;
            const auto bytes = Text::embedded_default_font_bytes();
            if (auto loaded = Text::Font::load(bytes)) {
                default_font_ = std::make_unique<Text::Font>(std::move(*loaded));
                register_font(0, *default_font_);
            }
        }
        return find_font(0);
    }

    /// Performs the font stack operation for `UI` using the supplied arguments.
    ///
    /// @param font_id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const Text::FontStack *TextBridge::font_stack(FontId font_id) const noexcept {
        const FontEntry *entry = find_font(font_id);
        return entry ? &entry->stack : nullptr;
    }

    /// Returns the current or globally available begin frame value.
    ///
    /// @return Returns the current begin frame value.
    /// @note This function does not throw exceptions.
    void TextBridge::begin_frame() noexcept { cache_.clear(); }

    /// Shapes and cache using the supplied arguments and current state.
    ///
    /// @param style `style` value used by the operation.
    /// @param utf8_content `utf8_content` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    const CachedShape *TextBridge::shape_and_cache(const TextStyle &style, string_view utf8_content) {
        const FontEntry *entry = find_font(style.font_id);
        if (entry == nullptr && style.font_id == 0 && load_default_font_) {
            entry = ensure_default_font();
        }
        if (entry == nullptr) {
            if (reported_missing_fonts_.insert(style.font_id).second) {
                Foundation::log_warn("UI text uses font id {} but no font is registered under it; the text will not be drawn", style.font_id);
            }
            return nullptr;
        }
        if (entry->stack.primary == nullptr) {
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
        const UString features{entry->features};
        auto shaped = entry->features.empty()
                          ? Text::shape_line_with_fallback(entry->stack, content.as_ustr())
                          : Text::shape_line_with_fallback(entry->stack, content.as_ustr(), features.as_ustr());
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

    /// Performs the measure operation for `UI` using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param config Configuration values controlling the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

    /// Handles the measure callback callback and updates the associated platform state.
    ///
    /// @param text Text consumed by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param user_data Data consumed or referenced by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    Clay_Dimensions TextBridge::measure_callback(Clay_StringSlice text, Clay_TextElementConfig *config, void *user_data) {
        if (config == nullptr || user_data == nullptr) {
            return Clay_Dimensions{};
        }
        return static_cast<TextBridge *>(user_data)->measure(text, *config);
    }

} // namespace SFT::UI
