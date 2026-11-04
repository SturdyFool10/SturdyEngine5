/// C ABI for `SFT::Text::Document`: an editable, versioned UTF-8 rope with undo/redo, position mapping, word and bracket navigation and search.
///
/// The document is wrapped in a record that also keeps the change list of the last edit, so the per-change texts (which are owned strings in the C++
/// API) can be read back by index without the ABI having to hand out lifetimes.

#include <Foundation/Foundation.hpp>

#include <memory>
#include <string>
#include <string_view>

#include <Renderer/Text/Document.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace T = SFT::Text;

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::mint_handle;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::revoke_handle;
    using SFT::Ffi::set_error;

    struct DocumentRecord {
        explicit DocumentRecord(std::string_view initial) : document(initial) {}
        T::Document document;
        T::ChangeSet last_changes;
    };

    [[nodiscard]] SturdyResult invalid(const char *what) noexcept { return set_error(STURDY_ERROR_INVALID_ARGUMENT, what); }

    T::ByteOffset offset_of(uint64_t value) noexcept { return T::ByteOffset{static_cast<SFT::usize>(value)}; }
    SturdyByteRange range_of(const T::TextRange &range) noexcept { return {range.start.value, range.end.value}; }

    template <class Body>
    [[nodiscard]] SturdyResult with_document(SturdyDocument handle, Body &&body) noexcept {
        return guarded([&]() -> SturdyResult {
            void *pointer = nullptr;
            const SturdyResult resolved = resolve_handle(handle.token, HandleKind::Document, &pointer);
            if (resolved != STURDY_OK) {
                return resolved;
            }
            return body(*static_cast<DocumentRecord *>(pointer));
        });
    }

    /// An offset past the end of the text is a caller error; inside it the C++ API clips or maps as documented.
    [[nodiscard]] SturdyResult check_offset(const DocumentRecord &record, uint64_t offset) noexcept {
        if (offset > record.document.snapshot().summary().bytes) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "offset is past the end of the document");
        }
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult finish(DocumentRecord &record, T::ApplyResult &&applied, SturdyApplyResult *out) {
        record.last_changes = std::move(applied.changes);
        if (out != nullptr) {
            *out = {record.last_changes.before.value, record.last_changes.after.value, static_cast<uint32_t>(record.last_changes.changes.size())};
        }
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult change_at(const DocumentRecord &record, uint32_t index, const T::Change **out) noexcept {
        if (index >= record.last_changes.changes.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "change index is out of range");
        }
        *out = &record.last_changes.changes[index];
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_document_create(const char *initial, size_t initial_size, SturdyDocument *out_document) {
    return guarded([&]() -> SturdyResult {
        if (out_document == nullptr || (initial == nullptr && initial_size != 0)) {
            return invalid("output pointer must not be null, and initial may only be null with size 0");
        }
        const std::string_view text{initial != nullptr ? initial : "", initial_size};
        std::unique_ptr<DocumentRecord> record;
        try {
            record = std::make_unique<DocumentRecord>(text);
        } catch (const std::invalid_argument &) {
            return invalid("the initial text is not valid UTF-8");
        }
        out_document->token = mint_handle(HandleKind::Document, record.get());
        static_cast<void>(record.release());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_destroy(SturdyDocument document) {
    return guarded([&]() -> SturdyResult {
        void *pointer = nullptr;
        const SturdyResult resolved = resolve_handle(document.token, HandleKind::Document, &pointer);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        revoke_handle(document.token);
        delete static_cast<DocumentRecord *>(pointer);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_revision(SturdyDocument document, uint64_t *out_revision) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_revision == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_revision = record.document.revision().value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_text(SturdyDocument document, char *buffer, size_t capacity, size_t *out_length) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        return copy_string_out(record.document.snapshot().flatten(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_slice(SturdyDocument document, SturdyByteRange range, char *buffer, size_t capacity, size_t *out_length) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        const T::DocumentSnapshot snapshot = record.document.snapshot();
        const SFT::usize bytes = snapshot.summary().bytes;
        if (range.start > range.end || range.end > bytes) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "range is not inside the document");
        }
        return copy_string_out(snapshot.slice(T::TextRange{offset_of(range.start), offset_of(range.end)}).flatten(), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_line_count(SturdyDocument document, uint64_t *out_count) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_count == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_count = record.document.snapshot().line_count();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_line_range(SturdyDocument document, uint64_t line, SturdyByteRange *out_range) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_range == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto range = record.document.snapshot().line_range(line);
        if (!range) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "line is past the last line");
        }
        *out_range = range_of(*range);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_stats(SturdyDocument document, SturdyDocumentStats *out_stats) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_stats == nullptr) {
            return invalid("output pointer must not be null");
        }
        const T::DocumentMemoryStats memory = record.document.memory_stats();
        const T::TextSummary summary = record.document.snapshot().summary();
        *out_stats = {memory.logical_bytes, memory.backing_storage_bytes, memory.tree_nodes,       memory.pieces,   memory.maximum_tree_depth,
                      memory.undo_revisions, memory.redo_revisions,       summary.scalars,         summary.utf16_code_units, summary.newlines,
                      summary.ascii ? STURDY_TRUE : STURDY_FALSE};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_apply(SturdyDocument document, const SturdyTextEdit *edits, uint32_t edit_count, SturdyEditKind kind,
                                                   SturdyApplyResult *out_result) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (edits == nullptr && edit_count != 0) {
            return invalid("edits must not be null");
        }
        T::EditKind native_kind{};
        switch (kind) {
        case STURDY_EDIT_STANDALONE:
            native_kind = T::EditKind::Standalone;
            break;
        case STURDY_EDIT_TYPING:
            native_kind = T::EditKind::Typing;
            break;
        case STURDY_EDIT_DELETION:
            native_kind = T::EditKind::Deletion;
            break;
        case STURDY_EDIT_KIND_FORCE_U32:
        default:
            return invalid("unrecognized edit kind");
        }
        T::EditTransaction transaction{record.document.revision()};
        for (uint32_t i = 0; i < edit_count; ++i) {
            if (edits[i].replacement == nullptr && edits[i].replacement_size != 0) {
                return invalid("an edit has a null replacement with a non-zero size");
            }
            transaction.replace(T::TextRange{offset_of(edits[i].range.start), offset_of(edits[i].range.end)},
                                std::string_view{edits[i].replacement != nullptr ? edits[i].replacement : "", edits[i].replacement_size});
        }
        auto applied = record.document.apply(transaction, native_kind);
        if (!applied) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, std::string{T::to_string(applied.error())});
        }
        return finish(record, std::move(*applied), out_result);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_undo(SturdyDocument document, SturdyApplyResult *out_result) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        auto result = record.document.undo();
        if (!result) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "nothing to undo");
        }
        return finish(record, std::move(*result), out_result);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_redo(SturdyDocument document, SturdyApplyResult *out_result) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        auto result = record.document.redo();
        if (!result) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "nothing to redo");
        }
        return finish(record, std::move(*result), out_result);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_change(SturdyDocument document, uint32_t index, SturdyDocumentChange *out_change) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_change == nullptr) {
            return invalid("output pointer must not be null");
        }
        const T::Change *change = nullptr;
        if (const SturdyResult found = change_at(record, index, &change); found != STURDY_OK) {
            return found;
        }
        *out_change = {range_of(change->old_range), range_of(change->new_range)};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_change_text(SturdyDocument document, uint32_t index, SturdyChangeText which, char *buffer, size_t capacity,
                                                         size_t *out_length) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        const T::Change *change = nullptr;
        if (const SturdyResult found = change_at(record, index, &change); found != STURDY_OK) {
            return found;
        }
        switch (which) {
        case STURDY_CHANGE_TEXT_INSERTED:
            return copy_string_out(change->inserted_text.cpp_string_view(), buffer, capacity, out_length);
        case STURDY_CHANGE_TEXT_DELETED:
            return copy_string_out(change->deleted_text.cpp_string_view(), buffer, capacity, out_length);
        case STURDY_CHANGE_TEXT_FORCE_U32:
        default:
            return invalid("unrecognized change text selector");
        }
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_offset_to_point(SturdyDocument document, uint64_t offset, SturdyTextPoint *out_point) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_point == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto point = record.document.snapshot().offset_to_point(offset_of(offset));
        if (!point) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "offset is not a character boundary inside the document");
        }
        *out_point = {point->line, point->scalar_column};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_offset_to_utf16(SturdyDocument document, uint64_t offset, SturdyTextPoint *out_point) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_point == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto point = record.document.snapshot().offset_to_utf16(offset_of(offset));
        if (!point) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "offset is not a character boundary inside the document");
        }
        *out_point = {point->line, point->code_unit_column};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_point_to_offset(SturdyDocument document, SturdyTextPoint point, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto offset = record.document.snapshot().point_to_offset(T::TextPoint{point.line, point.column});
        if (!offset) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "the position is past the end of its line or the document");
        }
        *out_offset = offset->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_utf16_to_offset(SturdyDocument document, SturdyTextPoint point, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        const auto offset = record.document.snapshot().utf16_to_offset(T::Utf16Point{point.line, point.column});
        if (!offset) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "the position is past the end of its line or the document");
        }
        *out_offset = offset->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_clip_offset(SturdyDocument document, uint64_t offset, SturdyBool after, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_offset = record.document.snapshot().clip_offset(offset_of(offset), after != 0 ? T::Bias::After : T::Bias::Before).value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_char_class_at(SturdyDocument document, uint64_t offset, SturdyCharClass *out_class) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_class == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, offset); checked != STURDY_OK) {
            return checked;
        }
        switch (record.document.snapshot().char_class_at(offset_of(offset))) {
        case T::CharClass::Whitespace:
            *out_class = STURDY_CHAR_CLASS_WHITESPACE;
            break;
        case T::CharClass::Word:
            *out_class = STURDY_CHAR_CLASS_WORD;
            break;
        case T::CharClass::Punctuation:
            *out_class = STURDY_CHAR_CLASS_PUNCTUATION;
            break;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_word_range_at(SturdyDocument document, uint64_t offset, SturdyByteRange *out_range) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_range == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, offset); checked != STURDY_OK) {
            return checked;
        }
        const auto range = record.document.snapshot().word_range_at(offset_of(offset));
        if (!range) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "there is no word at that offset");
        }
        *out_range = range_of(*range);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_next_word_boundary(SturdyDocument document, uint64_t offset, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, offset); checked != STURDY_OK) {
            return checked;
        }
        *out_offset = record.document.snapshot().next_word_boundary(offset_of(offset)).value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_previous_word_boundary(SturdyDocument document, uint64_t offset, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, offset); checked != STURDY_OK) {
            return checked;
        }
        *out_offset = record.document.snapshot().previous_word_boundary(offset_of(offset)).value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_matching_bracket(SturdyDocument document, uint64_t offset, uint64_t *out_offset) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_offset == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, offset); checked != STURDY_OK) {
            return checked;
        }
        const auto match = record.document.snapshot().matching_bracket(offset_of(offset));
        if (!match) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "there is no matching bracket");
        }
        *out_offset = match->value;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_find(SturdyDocument document, const char *pattern, uint64_t from, SturdyBool forward, SturdyBool case_sensitive,
                                                  SturdyByteRange *out_range) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (pattern == nullptr || out_range == nullptr) {
            return invalid("pattern and output pointer must not be null");
        }
        if (const SturdyResult checked = check_offset(record, from); checked != STURDY_OK) {
            return checked;
        }
        const auto range = record.document.snapshot().find(std::string_view{pattern}, offset_of(from), forward != 0, case_sensitive != 0);
        if (!range) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no match");
        }
        *out_range = range_of(*range);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_find_all(SturdyDocument document, const char *pattern, SturdyBool case_sensitive, SturdyByteRange *out_ranges,
                                                      uint32_t capacity, uint32_t *out_count) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (pattern == nullptr || out_count == nullptr) {
            return invalid("pattern and out_count must not be null");
        }
        const auto matches = record.document.snapshot().find_all(std::string_view{pattern}, case_sensitive != 0);
        *out_count = static_cast<uint32_t>(matches.size());
        if (out_ranges == nullptr) {
            return STURDY_OK;
        }
        const uint32_t count = *out_count < capacity ? *out_count : capacity;
        for (uint32_t i = 0; i < count; ++i) {
            out_ranges[i] = range_of(matches[i]);
        }
        return *out_count > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "output buffer is smaller than the list") : STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_is_line_blank(SturdyDocument document, uint64_t line, SturdyBool *out_blank) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_blank == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (line >= record.document.snapshot().line_count()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "line is past the last line");
        }
        *out_blank = record.document.snapshot().is_line_blank(line) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_line_indent(SturdyDocument document, uint64_t line, SturdyLineIndent *out_indent) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_indent == nullptr) {
            return invalid("output pointer must not be null");
        }
        if (line >= record.document.snapshot().line_count()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "line is past the last line");
        }
        const T::LineIndent indent = record.document.snapshot().line_indent(line);
        *out_indent = {indent.whitespace_bytes, indent.whitespace_scalars, indent.has_tabs ? STURDY_TRUE : STURDY_FALSE, indent.has_spaces ? STURDY_TRUE : STURDY_FALSE};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_detect_indent(SturdyDocument document, SturdyIndentStyle *out_style) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_style == nullptr) {
            return invalid("output pointer must not be null");
        }
        const T::IndentStyle style = record.document.snapshot().detect_indent_style();
        *out_style = {style.kind == T::IndentKind::Tabs ? STURDY_TRUE : STURDY_FALSE, style.width};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_document_detect_line_ending(SturdyDocument document, SturdyLineEnding *out_ending) {
    return with_document(document, [&](DocumentRecord &record) -> SturdyResult {
        if (out_ending == nullptr) {
            return invalid("output pointer must not be null");
        }
        *out_ending = record.document.snapshot().detect_line_ending() == T::LineEnding::CrLf ? STURDY_LINE_ENDING_CRLF : STURDY_LINE_ENDING_LF;
        return STURDY_OK;
    });
}

} // extern "C"
