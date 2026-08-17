#pragma once

#include <Text/src/Text/Text.hpp>

#include <algorithm>

#include "Context.hpp"
#include "ScrollArea.hpp"
#include "Style.hpp"
#include "TextEdit.hpp"

/// Document-backed multiline editor for source files and other large text. Unlike text_area()
/// (which remains ideal for short form content), this path virtualizes hard lines: each frame emits
/// only the viewport's logical lines plus two fixed-height spacers. The Document itself owns UTF-8,
/// snapshots, revisions, and edits; this state owns only widget-local cursor/focus animation.
namespace SFT::UI {

    class DocumentTextAreaState {
      public:
        [[nodiscard]] Text::Document &document() noexcept;
        [[nodiscard]] const Text::Document &document() const noexcept;
        [[nodiscard]] Text::ByteOffset caret() const noexcept;
        [[nodiscard]] bool focused() const noexcept;
        void set_focused(bool focused) noexcept;

        void set_text(string_view utf8);

      public:
        Text::Document document_{};
        Text::ByteOffset caret_{};
        bool focused_ = false;
        ColorTransition color_{};
        f32 blink_elapsed_ = 0.0f;
    };

    struct DocumentTextAreaResult {
        bool changed = false;
        bool focused = false;
        Text::Revision revision{};
        usize first_rendered_line = 0;
        usize rendered_line_count = 0;
    };

    namespace Detail {

        [[nodiscard]] Text::ByteOffset previous_scalar(const Text::DocumentSnapshot &snapshot,
                                                               Text::ByteOffset offset);

        [[nodiscard]] Text::ByteOffset next_scalar(const Text::DocumentSnapshot &snapshot,
                                                           Text::ByteOffset offset);

        bool replace_document_range(DocumentTextAreaState &state, Text::TextRange range,
                                           string_view replacement);

    } // namespace Detail

    /// Large-file multiline overload. `line_height` must match the line box expected by the caller's
    /// font/style; it is deliberately explicit because this virtualizer does not ask the shaper to
    /// lay out offscreen text. With wrapping enabled this fast path is not applicable, because one
    /// logical line may occupy an unknown number of visual rows; use the ordinary text_area() until
    /// a cached wrapping/layout layer is introduced.
    [[nodiscard]] DocumentTextAreaResult text_area(Context &ctx, const ElementDecl &decl,
                                                           const TextEditStyle &style, DocumentTextAreaState &state,
                                                           const TextEditInput &input, f32 delta_seconds,
                                                           const ScrollbarStyle &scrollbar_style,
                                                           ScrollAreaState &scroll_state, f32 line_height,
                                                           bool enabled = true);

} // namespace SFT::UI
