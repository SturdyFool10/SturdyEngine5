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

    /// Strong coordinate types. A value of one type is never implicitly accepted as another unit.
    struct ByteOffset { usize value = 0; [[nodiscard]] friend constexpr bool operator==(ByteOffset, ByteOffset) = default; };
    struct ScalarOffset { usize value = 0; [[nodiscard]] friend constexpr bool operator==(ScalarOffset, ScalarOffset) = default; };
    struct Revision { u64 value = 0; [[nodiscard]] friend constexpr bool operator==(Revision, Revision) = default; };

    /// Logical line plus Unicode-scalar column, both zero based. This intentionally is not a display
    /// coordinate; wrapping, tabs, bidi, and font shaping belong above the document layer.
    struct TextPoint {
        usize line = 0;
        usize scalar_column = 0;
        [[nodiscard]] friend constexpr bool operator==(TextPoint, TextPoint) = default;
    };

    /// UTF-16 line plus code-unit column, both zero based. This is the coordinate shape an LSP
    /// adapter needs, without allowing any LSP protocol types into this package.
    struct Utf16Point {
        usize line = 0;
        usize code_unit_column = 0;
        [[nodiscard]] friend constexpr bool operator==(Utf16Point, Utf16Point) = default;
    };

    struct TextRange {
        ByteOffset start{};
        ByteOffset end{};
        [[nodiscard]] friend constexpr bool operator==(TextRange, TextRange) = default;
    };

    enum class AnchorBias : u8 { Before, After };

    /// Deliberately opaque logical position. Its representation is private so a future
    /// collaboration layer can replace offset tracking with operation or CRDT identity without
    /// invalidating document clients.
    class Anchor {
      public:
        [[nodiscard]] Revision revision() const noexcept;
        [[nodiscard]] AnchorBias bias() const noexcept;
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
        /// The line still open at the very start of this summary's range (needed so join() can tell
        /// whether a boundary line, formed by gluing one chunk's trailing partial line to the next
        /// chunk's leading partial line, is longer than either chunk's own longest line).
        usize first_line_bytes = 0;
        usize first_line_scalars = 0;
        usize first_line_utf16_code_units = 0;
        /// The longest line (by Unicode scalar count) anywhere in this range, and its own 0-based
        /// line index relative to the start of this range — not an absolute document line number
        /// until it is the *root* summary. Tracking this per-summary and folding it through join()
        /// (see Document.cpp) is what makes DocumentSnapshot::longest_line() an O(1) query instead of
        /// an O(line count) scan; this is the same "longest_row" dimension Zed's Rope tracks in its
        /// own TextSummary, for the same reason (sizing a horizontal scrollbar/minimap).
        usize longest_line = 0;
        usize longest_line_scalars = 0;
        [[nodiscard]] friend constexpr bool operator==(const TextSummary &, const TextSummary &) = default;
    };

    struct LongestLine { usize line = 0; usize scalars = 0; };

    /// What kind of token an offset falls in — the same three-way split every plain-text/IDE editor
    /// uses for word-boundary movement and double-click word selection (see CharClass below); it is
    /// deliberately not language-aware (no keyword/identifier/string distinctions — that needs a
    /// real tokenizer, out of scope here).
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

    [[nodiscard]] string_view to_string(DocumentError error) noexcept;

    struct Splice {
        TextRange range{};
        /// Borrowed UTF-8; it is validated and copied into immutable append storage when the
        /// transaction is applied. It need only outlive apply().
        string_view replacement;
    };

    class EditTransaction {
      public:
        explicit EditTransaction(Revision base_revision);
        void replace(TextRange range, string_view replacement);
        [[nodiscard]] Revision base_revision() const noexcept;
        [[nodiscard]] const vector<Splice> &splices() const noexcept;

      private:
        Revision base_revision_{};
        vector<Splice> splices_;
    };

    struct Change {
        TextRange old_range{};
        TextRange new_range{};
        UString inserted_text{};
        /// Retained only for transaction/undo history. Consumers producing incremental parser/LSP
        /// edits normally need old_range and inserted_text; undo needs the exact removed bytes.
        UString deleted_text{};
    };

    struct ChangeSet {
        Revision before{};
        Revision after{};
        vector<Change> changes;
    };

    namespace Detail { struct DocumentRoot; struct DocumentState; }

    /// A zero-copy logical range. Each chunk is a contiguous view into immutable original or insert
    /// storage. Consumers that require contiguous text can explicitly flatten(), while parsers and
    /// save paths iterate the chunks without copying the complete range.
    class TextSlice {
      public:
        struct Chunk { string_view bytes{}; };
        [[nodiscard]] const vector<Chunk> &chunks() const noexcept;
        [[nodiscard]] usize byte_size() const noexcept;
        [[nodiscard]] string flatten() const;

        /// Internal construction hook used by snapshot traversal. Supplying the owner keeps each
        /// returned view alive without exposing backing-storage identity.
        void append_chunk(string_view bytes, shared_ptr<const string> owner);

      private:
        friend class DocumentSnapshot;
        vector<Chunk> chunks_;
        vector<shared_ptr<const string>> owners_;
        usize byte_size_ = 0;
    };

    class DocumentSnapshot {
      public:
        DocumentSnapshot() = default;
        [[nodiscard]] Revision revision() const noexcept;
        [[nodiscard]] TextSummary summary() const noexcept;
        [[nodiscard]] usize byte_size() const noexcept;
        /// Logical lines are addressed entirely from B+ tree newline summaries. `line_range()`
        /// excludes its trailing LF; the final empty line after a trailing LF is represented.
        [[nodiscard]] usize line_count() const noexcept;
        [[nodiscard]] optional<TextRange> line_range(usize line) const;
        [[nodiscard]] TextSlice slice(TextRange range) const;
        [[nodiscard]] string flatten() const;
        [[nodiscard]] optional<TextPoint> offset_to_point(ByteOffset offset) const;
        [[nodiscard]] optional<Utf16Point> offset_to_utf16(ByteOffset offset) const;
        [[nodiscard]] optional<ByteOffset> point_to_offset(TextPoint point) const;
        [[nodiscard]] optional<ByteOffset> utf16_to_offset(Utf16Point point) const;
        [[nodiscard]] optional<Anchor> anchor_at(ByteOffset offset, AnchorBias bias = AnchorBias::After) const;
        [[nodiscard]] optional<ByteOffset> resolve(const Anchor &anchor) const;
        [[nodiscard]] optional<TextRange> resolve(const AnchorRange &range) const;






        /// Snaps an arbitrary byte offset (e.g. from screen-space hit-testing, or a caller-supplied
        /// value that may not itself be a valid scalar boundary) to the nearest valid one. Also
        /// clamps out-of-range offsets to [0, byte_size()], so callers never have to bounds-check
        /// before calling this.
        [[nodiscard]] ByteOffset clip_offset(ByteOffset offset, Bias bias = Bias::After) const noexcept;
        /// The document's end coordinate: {last line, that line's own scalar length}. O(1).
        [[nodiscard]] TextPoint max_point() const noexcept;
        /// The longest line anywhere in the document and its scalar length. O(1) — see
        /// TextSummary::longest_line's own doc comment for why this doesn't need to scan every line.
        [[nodiscard]] LongestLine longest_line() const noexcept;

        [[nodiscard]] CharClass char_class_at(ByteOffset offset) const;
        /// The word (or, if `offset` sits in a run of punctuation, the punctuation run) touching
        /// `offset`; an empty range at `offset` if it sits in whitespace. Matches the usual
        /// double-click-to-select-word convention.
        [[nodiscard]] optional<TextRange> word_range_at(ByteOffset offset) const;
        /// Ctrl+Right / Ctrl+Left-style word motion: skips any whitespace, then one run of a single
        /// CharClass, landing on the boundary after/before it.
        [[nodiscard]] ByteOffset next_word_boundary(ByteOffset offset) const;
        [[nodiscard]] ByteOffset previous_word_boundary(ByteOffset offset) const;

        [[nodiscard]] LineIndent line_indent(usize line) const;
        [[nodiscard]] bool is_line_blank(usize line) const;

        /// Scans for the bracket matching the one at `offset` (which must be one of `()[]{}`), in
        /// whichever direction that bracket opens. Purely lexical — it counts bracket depth without
        /// any awareness of strings or comments, which is the same "no LSP" limitation this whole
        /// surface accepts; a real language-aware match needs a tokenizer.
        [[nodiscard]] optional<ByteOffset> matching_bracket(ByteOffset offset) const;

        /// Plain substring search (optionally case-insensitive, ASCII-fold only), streamed over the
        /// tree's chunks rather than flattening the document. `find` starts at `from` and stops at
        /// the first match in the requested direction; `find_all` collects every non-overlapping
        /// match left to right.
        [[nodiscard]] optional<TextRange> find(string_view pattern, ByteOffset from, bool forward = true,
                                               bool case_sensitive = true) const;
        [[nodiscard]] vector<TextRange> find_all(string_view pattern, bool case_sensitive = true) const;

        /// Heuristics over a sample of leading lines — the same kind of "detect indentation"/"detect
        /// line ending" pass every IDE runs once when a file is opened.
        [[nodiscard]] IndentStyle detect_indent_style() const;
        [[nodiscard]] LineEnding detect_line_ending() const;

      private:
        friend class Document;
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

    /// Streams a snapshot in piece-sized contiguous views. The callback is never given storage
    /// identity and may stop the traversal by returning false; saving, search, and future parser
    /// adapters should prefer this to flatten().
    using TextChunkVisitor = std::function<bool(string_view)>;

    /// Drives undo grouping. `Standalone` (the default) always starts a fresh undo step, matching the
    /// previous behavior. `Typing`/`Deletion` let apply() fold a transaction into the *previous* undo
    /// step instead of pushing a new one, when that previous step was the same kind and exactly
    /// adjacent (this transaction's edit starts where the previous one's ended, for Typing; ends where
    /// the previous one's started, for Deletion) — the same "one undo per burst of typing/backspacing"
    /// grouping every real editor does, so holding Backspace for 50 characters is one undo(), not 50.
    /// Document itself has no clock and no opinion about *when* a burst ends; the caller (typically
    /// the UI layer, which already knows about keystroke timing/focus changes) decides per-call
    /// whether a transaction continues the current burst or starts a new one.
    enum class EditKind : u8 { Standalone, Typing, Deletion };

    class Document {
      public:
        explicit Document(string_view initial_utf8 = {});
        [[nodiscard]] DocumentSnapshot snapshot() const;
        [[nodiscard]] Revision revision() const noexcept;
        [[nodiscard]] expected<ApplyResult, DocumentError> apply(const EditTransaction &transaction,
                                                                  EditKind kind = EditKind::Standalone);
        [[nodiscard]] optional<ApplyResult> undo();
        [[nodiscard]] optional<ApplyResult> redo();
        [[nodiscard]] DocumentMemoryStats memory_stats() const;
        bool visit(TextRange range, const TextChunkVisitor &visitor) const;

        /// Translates short-lived offsets/ranges through one known ChangeSet. An overlap resolves to
        /// the edited range boundary chosen by bias; long-lived UI state should use Anchor instead.
        [[nodiscard]] static optional<ByteOffset> translate(ByteOffset offset, AnchorBias bias, const ChangeSet &changes);
        [[nodiscard]] static optional<TextRange> translate(TextRange range, const ChangeSet &changes);

      private:
        shared_ptr<Detail::DocumentState> state_;
        vector<shared_ptr<Detail::DocumentState>> redo_;
    };

} // namespace SFT::Text
