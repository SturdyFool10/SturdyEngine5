#include <Renderer/Text/Text.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <random>
#include <string>
#include <utility>

namespace {
using namespace SFT;
using namespace SFT::Text;

/// Performs the coordinates and snapshots operation using the supplied arguments.
///
/// @pre `first.byte_size() == std::string{"alpha\n😀e\u0301\n"}.size()`; debug builds assert if this precondition is violated.
/// @pre `first.offset_to_point(ByteOffset{6}) == TextPoint{1, 0}`; debug builds assert if this precondition is violated.
/// @pre `first.offset_to_utf16(ByteOffset{10}) == Utf16Point{1, 2}`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void coordinates_and_snapshots() {
    Document document{"alpha\n😀e\u0301\n"};
    const DocumentSnapshot first = document.snapshot();
    assert(first.byte_size() == std::string{"alpha\n😀e\u0301\n"}.size());
    assert(first.offset_to_point(ByteOffset{6}) == (TextPoint{1, 0}));
    assert(first.offset_to_utf16(ByteOffset{10}) == (Utf16Point{1, 2}));
    assert(first.point_to_offset(TextPoint{1, 1}) == ByteOffset{10});
    assert(first.utf16_to_offset(Utf16Point{1, 2}) == ByteOffset{10});
    assert(first.line_count() == 3);
    assert(first.line_range(1) == (TextRange{{6}, {13}}));
    assert(first.slice(*first.line_range(1)).flatten() == "😀e\u0301");

    const auto anchor = first.anchor_at(ByteOffset{6}, AnchorBias::After);
    assert(anchor);
    EditTransaction transaction{first.revision()};
    transaction.replace({{0}, {0}}, "// ");
    auto applied = document.apply(transaction);
    assert(applied);
    assert(first.flatten() == "alpha\n😀e\u0301\n");
    assert(applied->snapshot.flatten() == "// alpha\n😀e\u0301\n");
    assert(applied->snapshot.resolve(*anchor) == ByteOffset{9});
}

/// Performs the bulk edits and ranges operation using the supplied arguments.
///
/// @pre `applied`; debug builds assert if this precondition is violated.
/// @pre `applied->snapshot.flatten() == "1 two 3"`; debug builds assert if this precondition is violated.
/// @pre `applied->changes.changes.size() == 2`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void bulk_edits_and_ranges() {
    Document document{"one two three"};
    const DocumentSnapshot before = document.snapshot();
    EditTransaction transaction{before.revision()};
    transaction.replace({{0}, {3}}, "1");
    transaction.replace({{8}, {13}}, "3");
    auto applied = document.apply(transaction);
    assert(applied);
    assert(applied->snapshot.flatten() == "1 two 3");
    assert(applied->changes.changes.size() == 2);
    assert(applied->changes.changes[1].new_range == (TextRange{{6}, {7}}));
    const TextSlice slice = applied->snapshot.slice({{2}, {5}});
    assert(slice.flatten() == "two");

    EditTransaction invalid{applied->snapshot.revision()};
    invalid.replace({{1}, {4}}, "x");
    invalid.replace({{3}, {5}}, "y");
    assert(!document.apply(invalid));
}

/// Performs the undo redo streaming and metrics operation using the supplied arguments.
///
/// @pre `applied && applied->snapshot.flatten() == "1st\nsecond"`; debug builds assert if this precondition is violated.
/// @pre `streamed == "1st\nsecond"`; debug builds assert if this precondition is violated.
/// @pre `stats.logical_bytes == streamed.size()`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void undo_redo_streaming_and_metrics() {
    Document document{"first\nsecond"};
    EditTransaction transaction{document.revision()};
    transaction.replace({{0}, {5}}, "1st");
    auto applied = document.apply(transaction);
    assert(applied && applied->snapshot.flatten() == "1st\nsecond");
    std::string streamed;
    assert(document.visit({{0}, {document.snapshot().byte_size()}}, [&](std::string_view chunk) {
        streamed.append(chunk);
        return true;
    }));
    assert(streamed == "1st\nsecond");
    const DocumentMemoryStats stats = document.memory_stats();
    assert(stats.logical_bytes == streamed.size());
    assert(stats.tree_nodes != 0 && stats.pieces != 0 && stats.backing_storage_bytes >= stats.logical_bytes);
    const auto undone = document.undo();
    assert(undone && undone->snapshot.flatten() == "first\nsecond");
    const auto redone = document.redo();
    assert(redone && redone->snapshot.flatten() == "1st\nsecond");
}

/// Performs the randomized transactions match reference operation using the supplied arguments.
///
/// @pre `applied`; debug builds assert if this precondition is violated.
/// @pre `applied->snapshot.flatten() == reference`; debug builds assert if this precondition is violated.
/// @pre `point && applied->snapshot.point_to_offset(*point) == ByteOffset{offset}`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void randomized_transactions_match_reference() {
    Document document{};
    std::string reference;
    std::minstd_rand random{0x5eed};
    for (usize iteration = 0; iteration < 500; ++iteration) {
        std::vector<usize> boundaries{0};
        for (usize byte = 1; byte < reference.size(); ++byte) {
            if ((static_cast<unsigned char>(reference[byte]) & 0xc0u) != 0x80u) boundaries.push_back(byte);
        }
        boundaries.push_back(reference.size());
        const usize start_index = random() % boundaries.size();
        const usize end_index = start_index + random() % (boundaries.size() - start_index);
        const usize start = boundaries[start_index];
        const usize end = boundaries[end_index];
        const std::string replacement = random() % 5 == 0 ? "😀" : std::string{static_cast<char>('a' + random() % 26)};
        EditTransaction transaction{document.revision()};
        transaction.replace({{start}, {end}}, replacement);
        auto applied = document.apply(transaction);
        assert(applied);
        reference.replace(start, end - start, replacement);
        assert(applied->snapshot.flatten() == reference);
        for (usize offset = 0; offset <= reference.size(); ++offset) {


            if (offset != 0 && offset != reference.size() &&
                (static_cast<unsigned char>(reference[offset]) & 0xc0u) == 0x80u) continue;
            const auto point = applied->snapshot.offset_to_point({offset});
            assert(point && applied->snapshot.point_to_offset(*point) == ByteOffset{offset});
        }
    }
}

/// Performs the rejects malformed or mid scalar edits operation using the supplied arguments.
///
/// @pre `!rejected && rejected.error() == DocumentError::InvalidRange`; debug builds assert if this precondition is violated.
/// @pre `!invalid && invalid.error() == DocumentError::InvalidUtf8`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void rejects_malformed_or_mid_scalar_edits() {
    Document document{"😀"};
    const DocumentSnapshot snapshot = document.snapshot();
    EditTransaction split_scalar{snapshot.revision()};
    split_scalar.replace({{1}, {1}}, "x");
    const auto rejected = document.apply(split_scalar);
    assert(!rejected && rejected.error() == DocumentError::InvalidRange);

    EditTransaction invalid_utf8{snapshot.revision()};
    const char malformed[] = {static_cast<char>(0xc3), 'x'};
    invalid_utf8.replace({{0}, {0}}, std::string_view{malformed, sizeof(malformed)});
    const auto invalid = document.apply(invalid_utf8);
    assert(!invalid && invalid.error() == DocumentError::InvalidUtf8);
}


/// Performs the anchor resolves across many edits operation using the supplied arguments.
///
/// @pre `anchor`; debug builds assert if this precondition is violated.
/// @pre `applied`; debug builds assert if this precondition is violated.
/// @pre `applied->snapshot.resolve(*anchor) == ByteOffset{expected}`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void anchor_resolves_across_many_edits() {
    Document document{"0123456789"};
    const auto anchor = document.snapshot().anchor_at({5}, AnchorBias::Before);
    assert(anchor);
    usize expected = 5;
    for (usize i = 0; i < 6; ++i) {
        const usize at = i % 2 == 0 ? 0 : document.snapshot().byte_size();
        const std::string insert = i % 2 == 0 ? "<<" : ">>";
        EditTransaction t{document.revision()};
        t.replace({{at}, {at}}, insert);
        auto applied = document.apply(t);
        assert(applied);
        if (at <= expected) expected += insert.size();
        assert(applied->snapshot.resolve(*anchor) == ByteOffset{expected});
    }
}

/// Performs the clip offset and max point operation using the supplied arguments.
///
/// @pre `s.clip_offset({0}) == ByteOffset{0}`; debug builds assert if this precondition is violated.
/// @pre `s.clip_offset({6}) == ByteOffset{6}`; debug builds assert if this precondition is violated.
/// @pre `s.clip_offset({100}) == ByteOffset{6}`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void clip_offset_and_max_point() {
    Document document{"a😀b"};
    const DocumentSnapshot s = document.snapshot();
    assert(s.clip_offset({0}) == ByteOffset{0});
    assert(s.clip_offset({6}) == ByteOffset{6});
    assert(s.clip_offset({100}) == ByteOffset{6});
    assert(s.clip_offset({3}, Bias::Before) == ByteOffset{1});
    assert(s.clip_offset({3}, Bias::After) == ByteOffset{5});
    assert(s.max_point() == (TextPoint{0, 3}));
}

/// Performs the reference longest line operation using the supplied arguments.
///
/// @param text Text consumed by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
LongestLine reference_longest_line(const std::string &text) {
    usize line = 0, best_line = 0, best_len = 0, current_len = 0;
    for (char c : text) {
        if (c == '\n') {
            if (current_len > best_len) { best_len = current_len; best_line = line; }
            ++line;
            current_len = 0;
        } else {
            ++current_len;
        }
    }
    if (current_len > best_len) { best_len = current_len; best_line = line; }
    return LongestLine{best_line, best_len};
}


/// Performs the longest line tracks across leaf boundaries operation using the supplied arguments.
///
/// @pre `actual.line == expected.line`; debug builds assert if this precondition is violated.
/// @pre `actual.scalars == expected.scalars`; debug builds assert if this precondition is violated.
/// @pre `document.snapshot().max_point().line == document.snapshot().line_count() - 1`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void longest_line_tracks_across_leaf_boundaries() {
    std::string text;
    std::minstd_rand random{0x1234};
    for (usize line = 0; text.size() < 900'000; ++line) {
        const usize length = 10 + random() % 30;
        text.append(length, static_cast<char>('a' + line % 26));
        text += '\n';
    }
    const usize insert_at = text.size() / 2;
    text.insert(insert_at, std::string(5000, 'z'));

    Document document{text};
    const LongestLine expected = reference_longest_line(text);
    const LongestLine actual = document.snapshot().longest_line();
    assert(actual.line == expected.line);
    assert(actual.scalars == expected.scalars);
    assert(document.snapshot().max_point().line == document.snapshot().line_count() - 1);

    // And that it stays correct after a real edit (join() runs on every apply(), not just at
    // construction).
    const usize end = document.snapshot().byte_size();
    EditTransaction extend{document.revision()};
    const std::string huge_line(8000, 'w');
    extend.replace({{end}, {end}}, huge_line);
    auto applied = document.apply(extend);
    assert(applied);
    text += huge_line;
    const LongestLine expected2 = reference_longest_line(text);
    const LongestLine actual2 = applied->snapshot.longest_line();
    assert(actual2.line == expected2.line);
    assert(actual2.scalars == expected2.scalars);
}

[[nodiscard]] CharClass reference_classify(char c) noexcept {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return CharClass::Whitespace;
    if (c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0) return CharClass::Word;
    return CharClass::Punctuation;
}

[[nodiscard]] std::pair<usize, usize> reference_word_range(const std::string &text, usize offset) {
    usize probe = offset;
    CharClass cls;
    if (probe < text.size() && reference_classify(text[probe]) != CharClass::Whitespace) {
        cls = reference_classify(text[probe]);
    } else if (probe > 0 && reference_classify(text[probe - 1]) != CharClass::Whitespace) {
        cls = reference_classify(text[probe - 1]);
        --probe;
    } else {
        return {offset, offset};
    }
    usize start = probe, end = probe + 1;
    while (start > 0 && reference_classify(text[start - 1]) == cls) --start;
    while (end < text.size() && reference_classify(text[end]) == cls) ++end;
    return {start, end};
}

[[nodiscard]] usize reference_next_word_boundary(const std::string &text, usize offset) {
    usize i = offset;
    while (i < text.size() && reference_classify(text[i]) == CharClass::Whitespace) ++i;
    if (i >= text.size()) return i;
    const CharClass cls = reference_classify(text[i]);
    while (i < text.size() && reference_classify(text[i]) == cls) ++i;
    return i;
}

[[nodiscard]] usize reference_previous_word_boundary(const std::string &text, usize offset) {
    usize i = offset;
    while (i > 0 && reference_classify(text[i - 1]) == CharClass::Whitespace) --i;
    if (i == 0) return i;
    const CharClass cls = reference_classify(text[i - 1]);
    while (i > 0 && reference_classify(text[i - 1]) == cls) --i;
    return i;
}

void word_boundaries_match_reference() {
    const std::string sample = "  hello_world(foo, bar) -> baz+qux  \n  another_line here!!  ";
    std::string filler;
    for (usize i = 0; filler.size() < 600'000; ++i) filler += "pad" + std::to_string(i) + " ";
    const std::string text = filler + sample + filler;

    Document document{text};
    const DocumentSnapshot snapshot = document.snapshot();
    std::minstd_rand random{0xbeef};
    for (usize i = 0; i < 500; ++i) {


        const usize offset = i % 2 == 0 ? filler.size() + random() % (sample.size() + 1) : random() % (text.size() + 1);
        const auto [expected_start, expected_end] = reference_word_range(text, offset);
        const auto actual = snapshot.word_range_at({offset});
        assert(actual);
        assert(actual->start.value == expected_start && actual->end.value == expected_end);
        assert(snapshot.next_word_boundary({offset}).value == reference_next_word_boundary(text, offset));
        assert(snapshot.previous_word_boundary({offset}).value == reference_previous_word_boundary(text, offset));
    }
}

/// Performs the line indent and blank lines operation using the supplied arguments.
///
/// @pre `s.line_indent(0).whitespace_scalars == 0`; debug builds assert if this precondition is violated.
/// @pre `!s.is_line_blank(0)`; debug builds assert if this precondition is violated.
/// @pre `four.whitespace_scalars == 4 && four.has_spaces && !four.has_tabs`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void line_indent_and_blank_lines() {
    Document document{"no_indent\n    four_spaces\n\tone_tab\n  \n\t  mixed_indent\n"};
    const DocumentSnapshot s = document.snapshot();
    assert(s.line_indent(0).whitespace_scalars == 0);
    assert(!s.is_line_blank(0));
    const LineIndent four = s.line_indent(1);
    assert(four.whitespace_scalars == 4 && four.has_spaces && !four.has_tabs);
    const LineIndent tab = s.line_indent(2);
    assert(tab.whitespace_scalars == 1 && tab.has_tabs && !tab.has_spaces);
    assert(s.is_line_blank(3));
    const LineIndent mixed = s.line_indent(4);
    assert(mixed.whitespace_scalars == 3 && mixed.has_spaces && mixed.has_tabs);
}

/// Performs the matching bracket scans operation using the supplied arguments.
///
/// @pre `s.matching_bracket({3}) == ByteOffset{22}`; debug builds assert if this precondition is violated.
/// @pre `s.matching_bracket({22}) == ByteOffset{3}`; debug builds assert if this precondition is violated.
/// @pre `s.matching_bracket({7}) == ByteOffset{11}`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void matching_bracket_scans() {
    const std::string text = "foo(bar[baz]{qux(1,2)})";
    Document document{text};
    const DocumentSnapshot s = document.snapshot();
    assert(s.matching_bracket({3}) == ByteOffset{22});
    assert(s.matching_bracket({22}) == ByteOffset{3});
    assert(s.matching_bracket({7}) == ByteOffset{11});
    assert(s.matching_bracket({11}) == ByteOffset{7});
    assert(s.matching_bracket({12}) == ByteOffset{21});
    assert(s.matching_bracket({16}) == ByteOffset{20});
    assert(!s.matching_bracket({0}));

    Document unbalanced{"(foo"};
    assert(!unbalanced.snapshot().matching_bracket({0}));


    const std::string huge = "(" + std::string(10'000, 'x') + ")";
    Document big{huge};
    const DocumentSnapshot bs = big.snapshot();
    assert(bs.matching_bracket({0}) == ByteOffset{huge.size() - 1});
    assert(bs.matching_bracket({huge.size() - 1}) == ByteOffset{0});
}

void find_and_find_all_match_reference() {
    std::string text;
    for (usize i = 0; i < 500; ++i) text += "needle sits here, and needle sits there.\n";
    text.insert(16382, "NEEDLE"); // straddles a piece_target_bytes (16KiB) boundary
    Document document{text};
    const DocumentSnapshot s = document.snapshot();

    const auto first = s.find("needle", {0});
    assert(first && text.substr(first->start.value, 6) == "needle");

    const auto ci = s.find("NEEDLE", {0}, /*forward=*/true, /*case_sensitive=*/false);
    assert(ci && ci->start.value == first->start.value);

    const auto exact = s.find("NEEDLE", {0}, true, true);
    assert(exact && text.substr(exact->start.value, 6) == "NEEDLE");

    assert(!s.find("xyzzy", {0}));

    const auto all = s.find_all("needle", /*case_sensitive=*/true);
    usize reference_count = 0;
    for (usize pos = text.find("needle"); pos != std::string::npos; pos = text.find("needle", pos + 6)) ++reference_count;
    assert(all.size() == reference_count);
    for (const TextRange &range : all) assert(text.substr(range.start.value, 6) == "needle");

    const auto backward = s.find("needle", {text.size()}, /*forward=*/false);
    assert(backward && backward->start.value == text.rfind("needle"));
}

// Regression test for a real bug found during self-review: find_all()'s cross-chunk carry logic


void find_all_never_overlaps_across_piece_boundaries() {
    const auto reference_find_all = [](const std::string &text, const std::string &pattern) {
        vector<std::pair<usize, usize>> matches;
        usize pos = text.find(pattern);
        while (pos != std::string::npos) {
            matches.emplace_back(pos, pos + pattern.size());
            pos = text.find(pattern, pos + pattern.size());
        }
        return matches;
    };

    for (usize run_start = 16378; run_start <= 16386; ++run_start) {
        std::string text(run_start, 'x');
        text += std::string(9, 'a');
        text += std::string(20, 'y');

        Document document{text};
        const auto all = document.snapshot().find_all("aaa");
        const auto reference = reference_find_all(text, "aaa");
        assert(all.size() == reference.size());
        for (usize i = 0; i < all.size(); ++i) {
            assert(all[i].start.value == reference[i].first && all[i].end.value == reference[i].second);
        }
        for (usize i = 1; i < all.size(); ++i) assert(all[i].start.value >= all[i - 1].end.value);
    }
}

void detects_indent_style_and_line_ending() {
    Document spaces{"a\n  b\n  c\n    d\n"};
    const IndentStyle space_style = spaces.snapshot().detect_indent_style();
    assert(space_style.kind == IndentKind::Spaces && space_style.width == 2);

    Document tabs{"a\n\tb\n\tc\n"};
    assert(tabs.snapshot().detect_indent_style().kind == IndentKind::Tabs);

    Document crlf{"a\r\nb\r\nc\r\n"};
    assert(crlf.snapshot().detect_line_ending() == LineEnding::CrLf);

    Document lf{"a\nb\nc\n"};
    assert(lf.snapshot().detect_line_ending() == LineEnding::Lf);
}

void anchor_range_resolves_through_edits() {
    Document document{"hello world"};
    const DocumentSnapshot before = document.snapshot();


    const auto start_anchor = before.anchor_at({0}, AnchorBias::After);
    const auto end_anchor = before.anchor_at({5}, AnchorBias::After);
    assert(start_anchor && end_anchor);
    const AnchorRange range{*start_anchor, *end_anchor};

    EditTransaction transaction{document.revision()};
    transaction.replace({{0}, {0}}, ">> ");
    auto applied = document.apply(transaction);
    assert(applied);
    const auto resolved = applied->snapshot.resolve(range);
    assert(resolved && applied->snapshot.slice(*resolved).flatten() == "hello");
}


void undo_redo_groups_typing_and_deletion_bursts() {


    Document document{};
    for (char c : {'a', 'b', 'c'}) {
        const usize end = document.snapshot().byte_size();
        const std::string ch(1, c);
        EditTransaction t{document.revision()};
        t.replace({{end}, {end}}, ch);
        assert(document.apply(t, EditKind::Typing));
    }
    assert(document.snapshot().flatten() == "abc");
    auto undone = document.undo();
    assert(undone && undone->snapshot.flatten() == "");
    assert(undone->changes.changes.size() == 1);
    assert(undone->changes.changes.front().deleted_text.cpp_string() == "abc");
    auto redone = document.redo();
    assert(redone && redone->snapshot.flatten() == "abc");
    assert(redone->changes.changes.front().inserted_text.cpp_string() == "abc");
    assert(!document.redo());

    document = Document{};
    for (char c : {'a', 'b'}) {
        const usize end = document.snapshot().byte_size();
        const std::string ch(1, c);
        EditTransaction t{document.revision()};
        t.replace({{end}, {end}}, ch);
        assert(document.apply(t, EditKind::Typing));
    }
    {
        EditTransaction t{document.revision()};
        t.replace({{0}, {0}}, "X");
        assert(document.apply(t, EditKind::Standalone));
    }
    for (char c : {'c', 'd'}) {
        const usize end = document.snapshot().byte_size();
        const std::string ch(1, c);
        EditTransaction t{document.revision()};
        t.replace({{end}, {end}}, ch);
        assert(document.apply(t, EditKind::Typing));
    }
    assert(document.snapshot().flatten() == "Xabcd");
    auto u1 = document.undo();
    assert(u1 && u1->snapshot.flatten() == "Xab");
    auto u2 = document.undo();
    assert(u2 && u2->snapshot.flatten() == "ab");
    auto u3 = document.undo();
    assert(u3 && u3->snapshot.flatten() == "");
    assert(!document.undo());
    auto r1 = document.redo();
    assert(r1 && r1->snapshot.flatten() == "ab");
    auto r2 = document.redo();
    assert(r2 && r2->snapshot.flatten() == "Xab");
    auto r3 = document.redo();
    assert(r3 && r3->snapshot.flatten() == "Xabcd");

    document = Document{"abcdef"};
    for (usize i = 0; i < 3; ++i) {
        const usize end = document.snapshot().byte_size();
        EditTransaction t{document.revision()};
        t.replace({{end - 1}, {end}}, "");
        assert(document.apply(t, EditKind::Deletion));
    }
    assert(document.snapshot().flatten() == "abc");
    auto backspace_undo = document.undo();
    assert(backspace_undo && backspace_undo->snapshot.flatten() == "abcdef");
    assert(backspace_undo->changes.changes.front().inserted_text.cpp_string() == "def");
}


void anchors_resolve_correctly_across_coalesced_bursts() {
    Document document{"x"};


    const auto anchor = document.snapshot().anchor_at({0}, AnchorBias::After);
    assert(anchor);
    for (char c : {'a', 'b', 'c'}) {
        const usize end = document.snapshot().byte_size() - 1;
        const std::string ch(1, c);
        EditTransaction t{document.revision()};
        t.replace({{end}, {end}}, ch);
        assert(document.apply(t, EditKind::Typing));
    }
    assert(document.snapshot().flatten() == "abcx");
    const auto resolved = document.snapshot().resolve(*anchor);
    assert(resolved && *resolved == ByteOffset{3});
}


void randomized_large_document_transactions_match_reference() {
    std::string reference;
    reference.reserve(700'000);
    for (usize line = 0; reference.size() < 650'000; ++line) {
        reference += "line " + std::to_string(line) + " the quick brown fox jumps over the lazy dog\n";
    }
    Document document{reference};
    assert(document.snapshot().flatten() == reference);

    std::minstd_rand random{0xc0ffee};
    for (usize iteration = 0; iteration < 250; ++iteration) {
        EditTransaction transaction{document.revision()};
        const usize splice_count = 1 + random() % 3;
        vector<std::pair<usize, usize>> ranges;
        for (usize s = 0; s < splice_count; ++s) {
            const usize start = random() % (reference.size() + 1);
            const usize length = std::min(reference.size() - start, static_cast<usize>(random() % 64));
            bool overlaps = false;
            for (const auto &[existing_start, existing_length] : ranges) {
                if (start < existing_start + existing_length && existing_start < start + length) overlaps = true;
            }
            if (overlaps) continue;
            ranges.emplace_back(start, length);
        }
        std::ranges::sort(ranges);
        vector<std::string> replacements;
        replacements.reserve(ranges.size());
        for (usize s = 0; s < ranges.size(); ++s) {
            std::string replacement;
            const usize replacement_length = random() % 40;
            for (usize c = 0; c < replacement_length; ++c) replacement += static_cast<char>('a' + random() % 26);
            replacements.push_back(std::move(replacement));
        }
        for (usize s = 0; s < ranges.size(); ++s) {
            transaction.replace({{ranges[s].first}, {ranges[s].first + ranges[s].second}}, replacements[s]);
        }
        auto applied = document.apply(transaction);
        assert(applied);
        for (usize s = ranges.size(); s-- > 0;) reference.replace(ranges[s].first, ranges[s].second, replacements[s]);
        assert(applied->snapshot.flatten() == reference);
        assert(applied->snapshot.byte_size() == reference.size());
    }
}


void large_document_edits_stay_path_local() {
    using Clock = std::chrono::steady_clock;
    const auto build_repeated = [](usize target_bytes) {
        const std::string line = "the quick brown fox jumps over the lazy dog\n";
        std::string text;
        text.reserve(target_bytes + line.size());
        while (text.size() < target_bytes) text += line;
        return text;
    };

    const auto average_edit_seconds = [](const std::string &initial) {
        Document document{initial};
        std::minstd_rand random{0xf00d};
        constexpr usize edit_count = 300;
        const auto start = Clock::now();
        for (usize i = 0; i < edit_count; ++i) {
            const usize size = document.snapshot().byte_size();
            const usize at = random() % (size + 1);
            EditTransaction insert{document.revision()};
            insert.replace({{at}, {at}}, "XYZ");
            auto applied = document.apply(insert);
            assert(applied);
            EditTransaction remove{document.revision()};
            remove.replace({{at}, {at + 3}}, "");
            applied = document.apply(remove);
            assert(applied);
        }
        return std::chrono::duration<double>(Clock::now() - start).count() / static_cast<double>(edit_count);
    };

    const double small_avg = average_edit_seconds(build_repeated(200'000));
    const std::string large_text = build_repeated(20'000'000);
    const double large_avg = average_edit_seconds(large_text);

    // 100x more document bytes should not translate into anywhere near 100x more time per edit.
    // The generous 10x band separates "cost tracks tree depth" from "cost tracks document size"
    // while staying robust to machine/scheduling noise on shared CI runners.
    assert(large_avg < small_avg * 10.0 + 0.001);

    const DocumentMemoryStats stats = Document{large_text}.memory_stats();
    assert(stats.maximum_tree_depth <= 12);
}

} // namespace

int main() {
    coordinates_and_snapshots();
    bulk_edits_and_ranges();
    undo_redo_streaming_and_metrics();
    randomized_transactions_match_reference();
    rejects_malformed_or_mid_scalar_edits();
    randomized_large_document_transactions_match_reference();
    large_document_edits_stay_path_local();
    anchor_resolves_across_many_edits();
    clip_offset_and_max_point();
    longest_line_tracks_across_leaf_boundaries();
    word_boundaries_match_reference();
    line_indent_and_blank_lines();
    matching_bracket_scans();
    find_and_find_all_match_reference();
    find_all_never_overlaps_across_piece_boundaries();
    detects_indent_style_and_line_ending();
    anchor_range_resolves_through_edits();
    undo_redo_groups_typing_and_deletion_bursts();
    anchors_resolve_correctly_across_coalesced_bursts();
}
