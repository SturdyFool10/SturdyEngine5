#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <vector>
#pragma endregion

#include "Break.hpp"
#include "Error.hpp"
#include "FontFallback.hpp"

using std::optional;
using std::vector;

namespace SFT::Text {

    enum class TextAlignment {
        Start,
        Center,
        End,
    };

    struct TextLayoutOptions {
                                                                                                 
                                                               
        optional<f32> max_width_em;
        TextAlignment alignment = TextAlignment::Start;
        ShapeOptions shape;
                                                                                       
        UString break_language;
    };

    struct LaidOutLine {
                                                                                                 
                                                                                                  
        usize byte_start = 0;
        usize visible_byte_end = 0;
        usize byte_end = 0;
        bool mandatory_break_after = false;
        ShapedLine shaped;
        f32 offset_em = 0.0f;
    };

    struct TextLayout {
        vector<LaidOutLine> lines;
        f32 width_em = 0.0f;
                                                                                                  
                                                                                                    
                                          
        f32 ascender_em = 0.0f;
        f32 descender_em = 0.0f;
        f32 line_gap_em = 0.0f;
        f32 line_height_em = 1.0f;
        f32 height_em = 0.0f;
    };

                                                                                               
                                                                                         
                                                                                               
                                                                         
    [[nodiscard]] TextExpected<TextLayout> layout_text(const FontStack &fonts, const ustr &text,
                                                       const TextLayoutOptions &options = {});

} // namespace SFT::Text
