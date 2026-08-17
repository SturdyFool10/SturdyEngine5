#pragma once

#include <Foundation/src/Foundation.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace SFT::Text {

    using std::optional;
    using std::shared_ptr;
    using std::string;
    using std::string_view;
    using std::vector;


    /// Compares the operands for equality.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    struct ByteOffset { usize value = 0; [[nodiscard]] friend constexpr bool operator==(ByteOffset, ByteOffset) = default; };
    /// Compares the operands for equality.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    struct ScalarOffset { usize value = 0; [[nodiscard]] friend constexpr bool operator==(ScalarOffset, ScalarOffset) = default; };
    /// Compares the operands for equality.
    ///
    /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    struct Revision { u64 value = 0; [[nodiscard]] friend constexpr bool operator==(Revision, Revision) = default; };


    struct TextPoint {
        usize line = 0;
        usize scalar_column = 0;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(TextPoint, TextPoint) = default;
    };


    struct Utf16Point {
        usize line = 0;
        usize code_unit_column = 0;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(Utf16Point, Utf16Point) = default;
    };

    struct TextRange {
        ByteOffset start{};
        ByteOffset end{};
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(TextRange, TextRange) = default;
    };

    enum class AnchorBias : u8 { Before, After };


    class Anchor {
      public:
        /// Returns the current or globally available revision value.
        ///
        /// @return Returns the current revision value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Revision revision() const noexcept;
        /// Returns the current or globally available bias value.
        ///
        /// @return Returns the current bias value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] AnchorBias bias() const noexcept;
        /// Returns the current or globally available valid value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool valid() const noexcept;

      private:
        friend class Document;
        friend class DocumentSnapshot;
        Revision revision_{};
        ByteOffset offset_{};
        AnchorBias bias_ = AnchorBias::After;
        bool valid_ = false;
    };

    struct AnchorRange { Anchor start{}; Anchor end{}; };

    struct TextSummary {
        usize bytes = 0;
        usize scalars = 0;
        usize utf16_code_units = 0;
        usize newlines = 0;
        usize last_line_bytes = 0;
        usize last_line_scalars = 0;
        usize last_line_utf16_code_units = 0;
        bool ascii = true;


        usize first_line_bytes = 0;
        usize first_line_scalars = 0;
        usize first_line_utf16_code_units = 0;


        usize longest_line = 0;
        usize longest_line_scalars = 0;
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] friend constexpr bool operator==(const TextSummary &, const TextSummary &) = default;
    };

    struct LongestLine { usize line = 0; usize scalars = 0; };


    enum class CharClass : u8 { Whitespace, Word, Punctuation };

    enum class Bias : u8 { Before, After };

    enum class IndentKind : u8 { Spaces, Tabs };
    struct IndentStyle { IndentKind kind = IndentKind::Spaces; usize width = 4; };

    enum class LineEnding : u8 { Lf, CrLf };

    struct LineIndent {
        usize whitespace_bytes = 0;
        usize whitespace_scalars = 0;
        bool has_tabs = false;
        bool has_spaces = false;
    };

    enum class DocumentError : u8 {
        InvalidUtf8,
        InvalidRange,
        InvalidBoundary,
        RevisionMismatch,
        OverlappingSplices,
    };

    /// Converts the value to string representation.
    ///
    /// @param error Error value describing the failure.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] string_view to_string(DocumentError error) noexcept;

    struct Splice {
        TextRange range{};


        string_view replacement;
    };

    class EditTransaction {
      public:
        /// Constructs a `EditTransaction` from the supplied initialization values.
        ///
        /// @param base_revision `base_revision` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit EditTransaction(Revision base_revision);
        /// Replaces the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param range Range of values to process.
        /// @param replacement `replacement` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void replace(TextRange range, string_view replacement);
        /// Returns the current or globally available base revision value.
        ///
        /// @return Returns the current base revision value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Revision base_revision() const noexcept;
        /// Returns the current or globally available splices value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<Splice> &splices() const noexcept;

      private:
        Revision base_revision_{};
        vector<Splice> splices_;
    };

    struct Change {
        TextRange old_range{};
        TextRange new_range{};
        UString inserted_text{};


        UString deleted_text{};
    };

    struct ChangeSet {
        Revision before{};
        Revision after{};
        vector<Change> changes;
    };

    namespace Detail { struct DocumentRoot; struct DocumentState; }


    class TextSlice {
      public:
        struct Chunk { string_view bytes{}; };
        /// Returns the current or globally available chunks value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const vector<Chunk> &chunks() const noexcept;
        /// Returns the byte size for this `TextSlice`.
        ///
        /// @return Returns the current byte size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_size() const noexcept;
        /// Returns the current or globally available flatten value.
        ///
        /// @return Returns the current flatten value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string flatten() const;


        /// Appends the supplied value or range to the current contents.
        ///
        /// @param bytes Size of the relevant data in bytes.
        /// @param owner Owner/context identifier used for validation or diagnostics.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void append_chunk(string_view bytes, shared_ptr<const string> owner);

      private:
        friend class DocumentSnapshot;
        vector<Chunk> chunks_;
        vector<shared_ptr<const string>> owners_;
        usize byte_size_ = 0;
    };

    class DocumentSnapshot {
      public:
        /// Constructs a `DocumentSnapshot` in its default state.
        ///
        /// @note This function does not throw exceptions.
        DocumentSnapshot() = default;
        /// Returns the current or globally available revision value.
        ///
        /// @return Returns the current revision value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Revision revision() const noexcept;
        /// Returns the current or globally available summary value.
        ///
        /// @return Returns the current summary value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextSummary summary() const noexcept;
        /// Returns the byte size for this `DocumentSnapshot`.
        ///
        /// @return Returns the current byte size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize byte_size() const noexcept;


        /// Returns the line count for this `DocumentSnapshot`.
        ///
        /// @return Returns the current line count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize line_count() const noexcept;
        /// Performs the line range operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param line `line` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<TextRange> line_range(usize line) const;
        /// Performs the slice operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] TextSlice slice(TextRange range) const;
        /// Returns the current or globally available flatten value.
        ///
        /// @return Returns the current flatten value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] string flatten() const;
        /// Performs the offset to point operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<TextPoint> offset_to_point(ByteOffset offset) const;
        /// Performs the offset to UTF-16 operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<Utf16Point> offset_to_utf16(ByteOffset offset) const;
        /// Computes the point to offset required by the supplied values.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ByteOffset> point_to_offset(TextPoint point) const;
        /// Computes the UTF-16 to offset required by the supplied values.
        ///
        /// @param point `point` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ByteOffset> utf16_to_offset(Utf16Point point) const;
        /// Performs the anchor at operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param bias `bias` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<Anchor> anchor_at(ByteOffset offset, AnchorBias bias = AnchorBias::After) const;
        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param anchor `anchor` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ByteOffset> resolve(const Anchor &anchor) const;
        /// Resolves the requested value into the concrete value used by the engine.
        ///
        /// @param range Range of values to process.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<TextRange> resolve(const AnchorRange &range) const;


        /// Computes the clip offset required by the supplied values.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param bias `bias` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ByteOffset clip_offset(ByteOffset offset, Bias bias = Bias::After) const noexcept;

        /// Returns the current or globally available max point value.
        ///
        /// @return Returns the current max point value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TextPoint max_point() const noexcept;


        /// Returns the current or globally available longest line value.
        ///
        /// @return Returns the current longest line value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] LongestLine longest_line() const noexcept;

        /// Performs the char class at operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] CharClass char_class_at(ByteOffset offset) const;


        /// Performs the word range at operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<TextRange> word_range_at(ByteOffset offset) const;


        /// Performs the next word boundary operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ByteOffset next_word_boundary(ByteOffset offset) const;
        /// Performs the previous word boundary operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ByteOffset previous_word_boundary(ByteOffset offset) const;

        /// Performs the line indent operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param line `line` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] LineIndent line_indent(usize line) const;
        /// Reports whether line blank holds for this `DocumentSnapshot`.
        ///
        /// @param line `line` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool is_line_blank(usize line) const;


        /// Performs the matching bracket operation for `DocumentSnapshot` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ByteOffset> matching_bracket(ByteOffset offset) const;


        /// Finds the requested entry in the available state.
        ///
        /// @param pattern `pattern` value used by the operation.
        /// @param from `from` value used by the operation.
        /// @param forward `forward` value used by the operation.
        /// @param case_sensitive `case_sensitive` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<TextRange> find(string_view pattern, ByteOffset from, bool forward = true,
                                               bool case_sensitive = true) const;
        /// Finds all in the available state.
        ///
        /// @param pattern `pattern` value used by the operation.
        /// @param case_sensitive `case_sensitive` value used by the operation.
        ///
        /// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<TextRange> find_all(string_view pattern, bool case_sensitive = true) const;


        /// Returns the current or globally available detect indent style value.
        ///
        /// @return Returns the current detect indent style value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] IndentStyle detect_indent_style() const;
        /// Returns the current or globally available detect line ending value.
        ///
        /// @return Returns the current detect line ending value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] LineEnding detect_line_ending() const;

      private:
        friend class Document;
        /// Constructs a `DocumentSnapshot` from the supplied initialization values.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit DocumentSnapshot(shared_ptr<const Detail::DocumentState> state);
        shared_ptr<const Detail::DocumentState> state_;
    };

    struct ApplyResult {
        DocumentSnapshot snapshot;
        ChangeSet changes;
    };

    struct DocumentMemoryStats {
        usize logical_bytes = 0;
        usize backing_storage_bytes = 0;
        usize tree_nodes = 0;
        usize pieces = 0;
        usize maximum_tree_depth = 0;
        usize undo_revisions = 0;
        usize redo_revisions = 0;
    };


    using TextChunkVisitor = std::function<bool(string_view)>;


    enum class EditKind : u8 { Standalone, Typing, Deletion };

    class Document {
      public:
        /// Constructs a `Document` from the supplied initialization values.
        ///
        /// @param initial_utf8 `initial_utf8` value used by the operation.
        ///
        /// @throws `invalid_argument` if `!info`.
        explicit Document(string_view initial_utf8 = {});
        /// Returns the current or globally available snapshot value.
        ///
        /// @return Returns the current snapshot value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] DocumentSnapshot snapshot() const;
        /// Returns the current or globally available revision value.
        ///
        /// @return Returns the current revision value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Revision revision() const noexcept;
        /// Applies the supplied operation or state to `Document`.
        ///
        /// @param transaction `transaction` value used by the operation.
        /// @param kind `kind` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `DocumentError::RevisionMismatch`, `DocumentError::InvalidRange`, `DocumentError::OverlappingSplices`, `DocumentError::InvalidUtf8`.
        [[nodiscard]] expected<ApplyResult, DocumentError> apply(const EditTransaction &transaction,
                                                                  EditKind kind = EditKind::Standalone);
        /// Returns the current or globally available undo value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ApplyResult> undo();
        /// Returns the current or globally available redo value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        [[nodiscard]] optional<ApplyResult> redo();
        /// Returns the current or globally available memory stats value.
        ///
        /// @return Returns the current memory stats value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] DocumentMemoryStats memory_stats() const;
        /// Visits the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param range Range of values to process.
        /// @param visitor `visitor` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        bool visit(TextRange range, const TextChunkVisitor &visitor) const;


        /// Translates the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param bias `bias` value used by the operation.
        /// @param changes `changes` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] static optional<ByteOffset> translate(ByteOffset offset, AnchorBias bias, const ChangeSet &changes);
        /// Translates the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param range Range of values to process.
        /// @param changes `changes` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] static optional<TextRange> translate(TextRange range, const ChangeSet &changes);

      private:
        shared_ptr<Detail::DocumentState> state_;
        vector<shared_ptr<Detail::DocumentState>> redo_;
    };

} // namespace SFT::Text
