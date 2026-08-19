#pragma once

#include <Renderer/Text/Text.hpp>

#include <algorithm>

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/ScrollArea.hpp>
#include <Renderer/UI/Style.hpp>
#include <Renderer/UI/TextEdit.hpp>


namespace SFT::UI {

    class DocumentTextAreaState {
      public:
        /// Returns the current or globally available document value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Text::Document &document() noexcept;
        /// Returns the current or globally available document value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const Text::Document &document() const noexcept;
        /// Returns the current or globally available caret value.
        ///
        /// @return Returns the current caret value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Text::ByteOffset caret() const noexcept;
        /// Returns the current or globally available focused value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool focused() const noexcept;
        /// Sets the focused for this `DocumentTextAreaState`.
        ///
        /// @param focused `focused` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_focused(bool focused) noexcept;

        /// Sets the text for this `DocumentTextAreaState`.
        ///
        /// @param utf8 `utf8` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Performs the previous scalar operation using the supplied arguments.
        ///
        /// @param snapshot `snapshot` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Text::ByteOffset previous_scalar(const Text::DocumentSnapshot &snapshot,
                                                               Text::ByteOffset offset);

        /// Performs the next scalar operation using the supplied arguments.
        ///
        /// @param snapshot `snapshot` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Text::ByteOffset next_scalar(const Text::DocumentSnapshot &snapshot,
                                                           Text::ByteOffset offset);

        /// Replaces document range using the supplied arguments and current state.
        ///
        /// @param state `state` value used by the operation.
        /// @param range Range of values to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool replace_document_range(DocumentTextAreaState &state, Text::TextRange range,
                                           string_view replacement);

    } // namespace Detail


    /// Performs the text area operation for `UI` using the supplied arguments.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    /// @param input `input` value used by the operation.
    /// @param delta_seconds `delta_seconds` value used by the operation.
    /// @param scrollbar_style `scrollbar_style` value used by the operation.
    /// @param scroll_state `scroll_state` value used by the operation.
    /// @param line_height `line_height` value used by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] DocumentTextAreaResult text_area(Context &ctx, const ElementDecl &decl,
                                                           const TextEditStyle &style, DocumentTextAreaState &state,
                                                           const TextEditInput &input, f32 delta_seconds,
                                                           const ScrollbarStyle &scrollbar_style,
                                                           ScrollAreaState &scroll_state, f32 line_height,
                                                           bool enabled = true);

} // namespace SFT::UI
