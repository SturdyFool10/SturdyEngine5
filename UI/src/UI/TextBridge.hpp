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


namespace SFT::UI {


    struct CachedShape {
        Text::ShapedLine shaped;
        f32 width_px = 0.0f;
        f32 height_px = 0.0f;
        const Text::FontStack *fonts = nullptr;
    };

    class TextBridge {
      public:


        /// Registers font using the supplied arguments and current state.
        ///
        /// @param font_id Identifier of the target object or resource.
        /// @param font `font` value used by the operation.
        /// @param emoji_fallback Fallback value used when the primary value is unavailable.
        /// @param fallbacks Fallback value used when the primary value is unavailable.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void register_font(FontId font_id, const Text::Font &font, const Text::Font *emoji_fallback = nullptr,
                           span<const Text::Font *const> fallbacks = {});

        /// Performs the font stack operation for `TextBridge` using the supplied arguments.
        ///
        /// @param font_id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Text::FontStack *font_stack(FontId font_id) const noexcept;


        /// Performs the begin frame operation for `TextBridge` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void begin_frame() noexcept;


        /// Shapes and cache using the supplied arguments and current state.
        ///
        /// @param style `style` value used by the operation.
        /// @param utf8_content `utf8_content` value used by the operation.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] const CachedShape *shape_and_cache(const TextStyle &style, string_view utf8_content);


        /// Performs the measure callback operation for `TextBridge` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        /// @param config Configuration values controlling the operation.
        /// @param user_data Data consumed or referenced by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static Clay_Dimensions measure_callback(Clay_StringSlice text, Clay_TextElementConfig *config,
                                                               void *user_data);

      private:
        /// Performs the measure operation for `TextBridge` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        /// @param config Configuration values controlling the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Clay_Dimensions measure(Clay_StringSlice text, const Clay_TextElementConfig &config);

        struct FontEntry {
            FontId id = 0;
            Text::FontStack stack{};


            vector<Text::FallbackFont> owned_fallbacks;
        };

        struct ShapeCacheKey {
            FontId font_id = 0;
            u16 font_size = 0;
            u16 letter_spacing = 0;
            string content;
            /// Compares the operands for equality.
            ///
            /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
            /// @note This function does not throw exceptions.
            [[nodiscard]] friend bool operator==(const ShapeCacheKey &, const ShapeCacheKey &) = default;
        };

        struct ShapeCacheKeyHash {
            /// Invokes the callable behavior provided by `ShapeCacheKeyHash`.
            ///
            /// @param key Key used to identify the requested entry.
            ///
            /// @return Returns the value produced by the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] usize operator()(const ShapeCacheKey &key) const noexcept;
        };

        /// Finds font in the available state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FontEntry *find_font(FontId id) const noexcept;

        vector<FontEntry> fonts_;
        unordered_map<ShapeCacheKey, CachedShape, ShapeCacheKeyHash> cache_;
    };

} // namespace SFT::UI
