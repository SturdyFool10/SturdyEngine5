#include "sturdy_rs/text.hpp"

#include <string>
#include <string_view>

// Generated from src/text.rs.
#include "sturdy_rs/text_bridge.h"

namespace sturdy_rs::text {
    namespace {

        namespace T = SFT::Text;

        std::string_view sv(rust::Str s) noexcept { return {s.data(), s.size()}; }
        T::ByteOffset off(std::uint64_t v) noexcept { return T::ByteOffset{static_cast<SFT::usize>(v)}; }
        ByteRange range_of(const T::TextRange &r) noexcept { return ByteRange{r.start.value, r.end.value}; }

        ApplyResultDesc to_result(const T::ApplyResult &applied) {
            ApplyResultDesc out{};
            out.ok = true;
            out.revision_before = applied.changes.before.value;
            out.revision_after = applied.changes.after.value;
            for (const T::Change &change : applied.changes.changes) {
                ChangeDesc c{};
                c.old_range = range_of(change.old_range);
                c.new_range = range_of(change.new_range);
                const auto inserted = change.inserted_text.cpp_string_view();
                const auto deleted = change.deleted_text.cpp_string_view();
                c.inserted = rust::String::lossy(inserted.data(), inserted.size());
                c.deleted = rust::String::lossy(deleted.data(), deleted.size());
                out.changes.push_back(std::move(c));
            }
            return out;
        }

    } // namespace

    std::unique_ptr<DocumentBox> document_new(rust::Str initial) {
        return std::make_unique<DocumentBox>(sv(initial));
    }
    std::uint64_t document_revision(const DocumentBox &d) { return d.revision().value; }
    rust::String document_text(const DocumentBox &d) {
        const std::string text = d.snapshot().flatten();
        return rust::String::lossy(text.data(), text.size());
    }
    rust::String document_slice(const DocumentBox &d, ByteRange r) {
        const std::string text = d.snapshot().slice(T::TextRange{off(r.start), off(r.end)}).flatten();
        return rust::String::lossy(text.data(), text.size());
    }
    std::uint64_t document_line_count(const DocumentBox &d) { return d.snapshot().line_count(); }
    ByteRange document_line_range(const DocumentBox &d, std::uint64_t line, bool &found) {
        const auto range = d.snapshot().line_range(line);
        found = range.has_value();
        return range ? range_of(*range) : ByteRange{};
    }
    DocumentStats document_stats(const DocumentBox &d) {
        const T::DocumentMemoryStats memory = d.memory_stats();
        const T::TextSummary summary = d.snapshot().summary();
        return DocumentStats{memory.logical_bytes, memory.backing_storage_bytes, memory.tree_nodes, memory.pieces,
                             memory.maximum_tree_depth, memory.undo_revisions, memory.redo_revisions,
                             summary.scalars, summary.utf16_code_units, summary.newlines, summary.ascii};
    }

    ApplyResultDesc document_apply(DocumentBox &d, const rust::Vec<EditDesc> &edits, std::uint8_t kind) {
        T::EditTransaction transaction{d.revision()};
        for (const EditDesc &edit : edits) {
            transaction.replace(T::TextRange{off(edit.range.start), off(edit.range.end)},
                                std::string_view{edit.replacement.data(), edit.replacement.size()});
        }
        auto applied = d.apply(transaction, kind == 1 ? T::EditKind::Typing : kind == 2 ? T::EditKind::Deletion
                                                                                        : T::EditKind::Standalone);
        if (!applied) {
            ApplyResultDesc out{};
            out.error = rust::String(std::string{T::to_string(applied.error())});
            return out;
        }
        return to_result(*applied);
    }
    ApplyResultDesc document_undo(DocumentBox &d) {
        auto result = d.undo();
        if (!result) {
            ApplyResultDesc out{};
            out.error = rust::String("nothing to undo");
            return out;
        }
        return to_result(*result);
    }
    ApplyResultDesc document_redo(DocumentBox &d) {
        auto result = d.redo();
        if (!result) {
            ApplyResultDesc out{};
            out.error = rust::String("nothing to redo");
            return out;
        }
        return to_result(*result);
    }

    PointDesc document_offset_to_point(const DocumentBox &d, std::uint64_t offset) {
        const auto p = d.snapshot().offset_to_point(off(offset));
        return p ? PointDesc{true, p->line, p->scalar_column} : PointDesc{};
    }
    PointDesc document_offset_to_utf16(const DocumentBox &d, std::uint64_t offset) {
        const auto p = d.snapshot().offset_to_utf16(off(offset));
        return p ? PointDesc{true, p->line, p->code_unit_column} : PointDesc{};
    }
    std::uint64_t document_point_to_offset(const DocumentBox &d, std::uint64_t line, std::uint64_t column, bool &found) {
        const auto o = d.snapshot().point_to_offset(T::TextPoint{line, column});
        found = o.has_value();
        return o ? o->value : 0;
    }
    std::uint64_t document_utf16_to_offset(const DocumentBox &d, std::uint64_t line, std::uint64_t column, bool &found) {
        const auto o = d.snapshot().utf16_to_offset(T::Utf16Point{line, column});
        found = o.has_value();
        return o ? o->value : 0;
    }
    std::uint64_t document_clip_offset(const DocumentBox &d, std::uint64_t offset, bool after) {
        return d.snapshot().clip_offset(off(offset), after ? T::Bias::After : T::Bias::Before).value;
    }
    std::uint8_t document_char_class_at(const DocumentBox &d, std::uint64_t offset) {
        return static_cast<std::uint8_t>(d.snapshot().char_class_at(off(offset)));
    }
    ByteRange document_word_range_at(const DocumentBox &d, std::uint64_t offset, bool &found) {
        const auto r = d.snapshot().word_range_at(off(offset));
        found = r.has_value();
        return r ? range_of(*r) : ByteRange{};
    }
    std::uint64_t document_next_word_boundary(const DocumentBox &d, std::uint64_t offset) {
        return d.snapshot().next_word_boundary(off(offset)).value;
    }
    std::uint64_t document_previous_word_boundary(const DocumentBox &d, std::uint64_t offset) {
        return d.snapshot().previous_word_boundary(off(offset)).value;
    }
    std::uint64_t document_matching_bracket(const DocumentBox &d, std::uint64_t offset, bool &found) {
        const auto o = d.snapshot().matching_bracket(off(offset));
        found = o.has_value();
        return o ? o->value : 0;
    }
    ByteRange document_find(const DocumentBox &d, rust::Str pattern, std::uint64_t from, bool forward,
                            bool case_sensitive, bool &found) {
        const auto r = d.snapshot().find(sv(pattern), off(from), forward, case_sensitive);
        found = r.has_value();
        return r ? range_of(*r) : ByteRange{};
    }
    rust::Vec<ByteRange> document_find_all(const DocumentBox &d, rust::Str pattern, bool case_sensitive) {
        rust::Vec<ByteRange> out;
        for (const T::TextRange &r : d.snapshot().find_all(sv(pattern), case_sensitive)) {
            out.push_back(range_of(r));
        }
        return out;
    }
    bool document_is_line_blank(const DocumentBox &d, std::uint64_t line) { return d.snapshot().is_line_blank(line); }
    std::array<std::uint64_t, 4> document_line_indent(const DocumentBox &d, std::uint64_t line) {
        const T::LineIndent i = d.snapshot().line_indent(line);
        return {i.whitespace_bytes, i.whitespace_scalars, i.has_tabs ? 1u : 0u, i.has_spaces ? 1u : 0u};
    }
    std::array<std::uint64_t, 2> document_detect_indent(const DocumentBox &d) {
        const T::IndentStyle s = d.snapshot().detect_indent_style();
        return {s.kind == T::IndentKind::Tabs ? 1u : 0u, s.width};
    }
    std::uint8_t document_detect_line_ending(const DocumentBox &d) {
        return d.snapshot().detect_line_ending() == T::LineEnding::CrLf ? 1 : 0;
    }

} // namespace sturdy_rs::text
