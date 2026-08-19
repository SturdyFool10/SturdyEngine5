#include <Renderer/Text/Break.hpp>

#include <graphemebreak.h>
#include <linebreak.h>
#include <wordbreak.h>

#include <algorithm>

namespace SFT::Text {

namespace {

/// Performs the language pointer operation for `Text` using the supplied arguments.
///
/// @param language `language` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] const char *language_pointer(const optional<UString> &language) noexcept {
    return language ? language->c_str() : nullptr;
}

/// Performs the owned language operation for `Text` using the supplied arguments.
///
/// @param language `language` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<UString> owned_language(const ustr &language) {
    return language.empty() ? std::nullopt : optional<UString>{UString{language}};
}

template <int BreakValue>
[[nodiscard]] vector<usize> segmentation_boundaries(const ustr &text, const ustr &language,
                                                    void (*segment)(const utf8_t *, size_t, const char *, char *)) {
    const string_view bytes = text.cpp_string_view();
    vector<usize> boundaries{0};
    if (bytes.empty()) {
        return boundaries;
    }
    vector<char> breaks(bytes.size());
    const optional<UString> owned = owned_language(language);
    segment(reinterpret_cast<const utf8_t *>(bytes.data()), bytes.size(), language_pointer(owned), breaks.data());
    for (usize i = 0; i < breaks.size(); ++i) {
        if (breaks[i] == BreakValue) {
            boundaries.push_back(i + 1);
        }
    }
    if (boundaries.back() != bytes.size()) {
        boundaries.push_back(bytes.size());
    }
    return boundaries;
}

} // namespace

/// Performs the line break opportunities operation for `Text` using the supplied arguments.
///
/// @param text Text consumed by the operation.
/// @param language `language` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<LineBreakOpportunity> line_break_opportunities(const ustr &text, const ustr &language) {
    const string_view bytes = text.cpp_string_view();
    vector<LineBreakOpportunity> opportunities;
    if (bytes.empty()) {
        return opportunities;
    }

    vector<char> breaks(bytes.size());
    const optional<UString> owned = owned_language(language);
    set_linebreaks_utf8(reinterpret_cast<const utf8_t *>(bytes.data()), bytes.size(), language_pointer(owned),
                        breaks.data());
    for (usize i = 0; i < breaks.size(); ++i) {
        if (breaks[i] == LINEBREAK_ALLOWBREAK || breaks[i] == LINEBREAK_MUSTBREAK) {
            opportunities.push_back(LineBreakOpportunity{
                .byte_index = i + 1,
                .kind = breaks[i] == LINEBREAK_MUSTBREAK ? LineBreakKind::Mandatory : LineBreakKind::Allowed,
            });
        }
    }
    if (opportunities.empty() || opportunities.back().byte_index != bytes.size()) {
        opportunities.push_back(LineBreakOpportunity{.byte_index = bytes.size(), .kind = LineBreakKind::Allowed});
    }
    return opportunities;
}

/// Performs the grapheme boundaries operation for `Text` using the supplied arguments.
///
/// @param text Text consumed by the operation.
/// @param language `language` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<usize> grapheme_boundaries(const ustr &text, const ustr &language) {
    return segmentation_boundaries<GRAPHEMEBREAK_BREAK>(text, language, set_graphemebreaks_utf8);
}

/// Performs the word boundaries operation for `Text` using the supplied arguments.
///
/// @param text Text consumed by the operation.
/// @param language `language` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<usize> word_boundaries(const ustr &text, const ustr &language) {
    return segmentation_boundaries<WORDBREAK_BREAK>(text, language, set_wordbreaks_utf8);
}

} // namespace SFT::Text
