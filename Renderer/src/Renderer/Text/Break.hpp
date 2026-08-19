#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <vector>
#pragma endregion

using std::vector;

namespace SFT::Text {

    enum class LineBreakKind {
        Allowed,
        Mandatory,
    };

    struct LineBreakOpportunity {

        usize byte_index = 0;
        LineBreakKind kind = LineBreakKind::Allowed;
    };


    /// Performs the line break opportunities operation using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param language `language` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<LineBreakOpportunity> line_break_opportunities(const ustr &text,
                                                                        const ustr &language = {});


    /// Performs the grapheme boundaries operation using the supplied arguments.
    ///
    /// @param text Text consumed by the operation.
    /// @param language `language` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<usize> grapheme_boundaries(const ustr &text, const ustr &language = {});

    [[nodiscard]] vector<usize> word_boundaries(const ustr &text, const ustr &language = {});

} // namespace SFT::Text
