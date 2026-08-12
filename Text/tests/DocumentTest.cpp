#include <Text/src/Text/Text.hpp>

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

void coordinates_and_snapshots() {
    Document document{"alpha\n😀e\u0301\n"};
    const DocumentSnapshot first = document.snapshot();
    assert(first.byte_size() == std::string{"alpha\n😀e\u0301\n"}.size());
    assert(first.offset_to_point(ByteOffset{6}) == TextPoint{1, 0});
    assert(first.offset_to_utf16(ByteOffset{10}) == Utf16Point{1, 2});
    assert(first.point_to_offset(TextPoint{1, 1}) == ByteOffset{10});
    assert(first.utf16_to_offset(Utf16Point{1, 2}) == ByteOffset{10});
    assert(first.line_count() == 3);
    assert(first.line_range(1) == TextRange{{6}, {13}});
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
    assert(applied->changes.changes[1].new_range == TextRange{{6}, {7}});
    const TextSlice slice = applied->snapshot.slice({{2}, {5}});
    assert(slice.flatten() == "two");

    EditTransaction invalid{applied->snapshot.revision()};
    invalid.replace({{1}, {4}}, "x");
    invalid.replace({{3}, {5}}, "y");
    assert(!document.apply(invalid));
}

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
            // The reference generated here only uses ASCII plus complete inserted emoji, so valid
            // document boundaries are either byte 0/end, ASCII positions, or emoji edges.
            if (offset != 0 && offset != reference.size() &&
                (static_cast<unsigned char>(reference[offset]) & 0xc0u) == 0x80u) continue;
            const auto point = applied->snapshot.offset_to_point({offset});
            assert(point && applied->snapshot.point_to_offset(*point) == ByteOffset{offset});
        }
    }
}

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

// Regression test for a resolve() bug found while writing the undo-grouping tests below: resolving
// an anchor across more than one intervening edit must replay each edit's ChangeSet in the order it
// actually happened (oldest to newest), not in the order walking ->parent visits them (newest to
// oldest) — every previously-existing test only ever resolved across exactly one edit, so this path
// was completely untested before.
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

void clip_offset_and_max_point() {
    Document document{"a😀b"}; // 'a' (1 byte) + U+1F600 (4 bytes) + 'b' (1 byte) = 6 bytes
    const DocumentSnapshot s = document.snapshot();
    assert(s.clip_offset({0}) == ByteOffset{0});
    assert(s.clip_offset({6}) == ByteOffset{6});
    assert(s.clip_offset({100}) == ByteOffset{6}); // out-of-range clamps to the end
    assert(s.clip_offset({3}, Bias::Before) == ByteOffset{1}); // mid-emoji rounds back to its start
    assert(s.clip_offset({3}, Bias::After) == ByteOffset{5}); // ...or forward to its end
    assert(s.max_point() == TextPoint{0, 3}); // "a", the emoji (1 scalar), "b" => 3 scalars, one line
}

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

// Deliberately builds a document with multiple leaves/branch levels (piece_target_bytes=16KiB,
// leaf_piece_capacity=32) so the longest line ends up straddling a leaf boundary somewhere, which is
// exactly the case join()'s "boundary line" handling exists for — a longest-line query that only
// checked each chunk's own internal longest line would silently miss this.
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
        // Half the samples land in the handwritten segment (where class transitions are dense);
        // the rest are uniform over the whole (multi-leaf) document.
        const usize offset = i % 2 == 0 ? filler.size() + random() % (sample.size() + 1) : random() % (text.size() + 1);
        const auto [expected_start, expected_end] = reference_word_range(text, offset);
        const auto actual = snapshot.word_range_at({offset});
        assert(actual);
        assert(actual->start.value == expected_start && actual->end.value == expected_end);
        assert(snapshot.next_word_boundary({offset}).value == reference_next_word_boundary(text, offset));
        assert(snapshot.previous_word_boundary({offset}).value == reference_previous_word_boundary(text, offset));
    }
}

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
    const LineIndent mixed = s.line_indent(4); // leading "\t  " precedes "mixed_indent"
    assert(mixed.whitespace_scalars == 3 && mixed.has_spaces && mixed.has_tabs);
}

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

    // Exercise the windowed forward/backward scan spanning more than one 4KiB window.
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
// didn't track how much of the carried tail was already consumed by a previous match, so a match
// ending near a ~16KiB piece boundary inside a repetitive run could produce a second, overlapping
// match. Constructs that exact failure shape — a run of 'a's straddling piece_target_bytes (16384)
// at every possible offset — against a pattern ("aaa") that only divides the run evenly in an
// overlapping way, and checks both against a reference and the non-overlap property directly.
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
    // Bias::After at the insertion point (offset 0) is what makes the anchor track "the start of
    // hello" through an insert landing exactly there, rather than staying pinned to absolute 0 —
    // Bias::Before would deliberately stay put, same as the existing anchor_at() convention tested
    // in coordinates_and_snapshots() above.
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

// Undo grouping (EditKind::Typing/Deletion) is the trickiest new piece: it restructures how many
// undo()/redo() calls a burst costs without collapsing the underlying revision chain, and composes a
// synthesized Change spanning the whole burst. All three properties get checked here: burst
// collapsing, group boundaries at a Standalone edit, and backspace (right-to-left) composition.
void undo_redo_groups_typing_and_deletion_bursts() {
    // Splice::replacement is a borrowed string_view that "need only outlive apply()" (see its own
    // doc comment in Document.hpp) — the char must be named in a local that outlives the apply()
    // call, not passed as a std::string temporary that replace() would otherwise bind a dangling
    // view to.
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

// Undo grouping folds undo()/redo() step *counting*, but must never collapse the actual revision
// chain — an Anchor created mid-burst still has to resolve correctly, both while the burst continues
// and after it. If it didn't, coalescing would be an observable behavior change, not just a UX one.
void anchors_resolve_correctly_across_coalesced_bursts() {
    Document document{"x"};
    // Bias::After is what makes the anchor track 'x' through inserts landing exactly at its
    // position, the same convention exercised in anchor_range_resolves_through_edits() above.
    const auto anchor = document.snapshot().anchor_at({0}, AnchorBias::After);
    assert(anchor);
    for (char c : {'a', 'b', 'c'}) {
        const usize end = document.snapshot().byte_size() - 1; // grow the "abc" prefix, just before 'x'
        const std::string ch(1, c);
        EditTransaction t{document.revision()};
        t.replace({{end}, {end}}, ch);
        assert(document.apply(t, EditKind::Typing));
    }
    assert(document.snapshot().flatten() == "abcx");
    const auto resolved = document.snapshot().resolve(*anchor);
    assert(resolved && *resolved == ByteOffset{3});
}

// The fuzz test above never grows past a few hundred bytes, so its tree never grows past a single
// leaf and split()/concat()'s branch-node code paths (everything beyond a lone leaf) go completely
// unexercised. This starts from a multi-leaf, multi-branch-level document instead, so every boundary
// case of a real split (offset landing exactly between two children, inside one, etc.) and both arms
// of concat's uneven-height spine descent get hit repeatedly.
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
            const usize length = std::min(reference.size() - start, random() % 64);
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

// Document::apply() is meant to be path-local: each edit is split() + concat() surgery bounded by
// tree depth (O(log n) node allocations), not a flatten-the-whole-tree-and-rebuild pass (O(n)). This
// exercises that guarantee two ways: per-edit wall-clock time should not scale with document size,
// and tree depth should stay small even for a large document.
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
