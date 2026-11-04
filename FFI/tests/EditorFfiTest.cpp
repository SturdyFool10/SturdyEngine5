/// Exercises the text document and dock workspace C ABI. The dock chrome needs an open UI frame, which a headless UI tree provides without a window or
/// graphics device, so this test mints an engine handle (static builds only).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Async/Scheduler.hpp>
#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "EditorFfiTest: %s\n", description);
            ++failures;
        }
    }

    std::string read_text(SturdyDocument document) {
        size_t length = 0;
        (void)sturdy_document_text(document, nullptr, 0, &length);
        std::string text(length, '\0');
        (void)sturdy_document_text(document, text.data(), text.size(), &length);
        text.resize(length - 1);
        return text;
    }

    void test_document() {
        SturdyDocument document{};
        check(sturdy_document_create(nullptr, 5, &document) == STURDY_ERROR_INVALID_ARGUMENT, "a null text with a size is refused");
        const char bad[] = {char(0xff), char(0xfe)};
        check(sturdy_document_create(bad, 2, &document) == STURDY_ERROR_INVALID_ARGUMENT, "invalid UTF-8 is refused");
        const std::string initial = "hello world\nsecond (line)\n";
        check(sturdy_document_create(initial.data(), initial.size(), &document) == STURDY_OK, "a document is created");
        check(read_text(document) == initial, "the text reads back");

        uint64_t revision = 99, lines = 0;
        check(sturdy_document_revision(document, &revision) == STURDY_OK && sturdy_document_line_count(document, &lines) == STURDY_OK && lines == 3, "three lines (the last empty)");
        SturdyByteRange range{};
        check(sturdy_document_line_range(document, 1, &range) == STURDY_OK && range.start == 12 && range.end == 25, "line 1 spans its bytes");
        check(sturdy_document_line_range(document, 9, &range) == STURDY_ERROR_OUT_OF_RANGE, "a line past the end is out of range");
        char slice[8] = {};
        size_t length = 0;
        check(sturdy_document_slice(document, {0, 5}, slice, sizeof(slice), &length) == STURDY_OK && std::strcmp(slice, "hello") == 0 && length == 6, "a slice reads back");
        check(sturdy_document_slice(document, {5, 1}, slice, sizeof(slice), &length) == STURDY_ERROR_OUT_OF_RANGE, "a reversed range is refused");
        check(sturdy_document_slice(document, {0, 9999}, slice, sizeof(slice), &length) == STURDY_ERROR_OUT_OF_RANGE, "a range past the end is refused");

        // Edit, undo, redo, and the change texts of the edit.
        SturdyTextEdit edit{{0, 5}, "HOWDY", 5};
        SturdyApplyResult applied{};
        check(sturdy_document_apply(document, &edit, 1, STURDY_EDIT_STANDALONE, &applied) == STURDY_OK && applied.change_count == 1 && applied.revision_after != applied.revision_before,
              "an edit applies and bumps the revision");
        check(read_text(document).rfind("HOWDY world", 0) == 0, "the edit changed the text");
        char inserted[16] = {}, deleted[16] = {};
        check(sturdy_document_change_text(document, 0, STURDY_CHANGE_TEXT_INSERTED, inserted, sizeof(inserted), nullptr) == STURDY_OK && std::strcmp(inserted, "HOWDY") == 0 &&
                  sturdy_document_change_text(document, 0, STURDY_CHANGE_TEXT_DELETED, deleted, sizeof(deleted), nullptr) == STURDY_OK && std::strcmp(deleted, "hello") == 0,
              "the change texts name what went in and what came out");
        SturdyDocumentChange change{};
        check(sturdy_document_change(document, 0, &change) == STURDY_OK && change.old_range.end == 5 && change.new_range.end == 5, "the change ranges are reported");
        check(sturdy_document_change(document, 1, &change) == STURDY_ERROR_OUT_OF_RANGE, "a change past the end is out of range");
        SturdyTextEdit overlapping[2] = {{{0, 4}, "a", 1}, {{2, 6}, "b", 1}};
        check(sturdy_document_apply(document, overlapping, 2, STURDY_EDIT_STANDALONE, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "overlapping edits are refused");
        SturdyTextEdit past_end{{0, 99999}, "x", 1};
        check(sturdy_document_apply(document, &past_end, 1, STURDY_EDIT_STANDALONE, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "an edit past the end is refused");
        check(read_text(document).rfind("HOWDY world", 0) == 0, "failed edits changed nothing");
        check(sturdy_document_undo(document, &applied) == STURDY_OK && read_text(document) == initial, "undo restores the text");
        check(sturdy_document_redo(document, &applied) == STURDY_OK && read_text(document).rfind("HOWDY", 0) == 0, "redo reapplies it");
        check(sturdy_document_undo(document, nullptr) == STURDY_OK && sturdy_document_undo(document, nullptr) == STURDY_ERROR_NOT_AVAILABLE, "undo runs out");

        // Position mapping and navigation.
        SturdyTextPoint point{};
        check(sturdy_document_offset_to_point(document, 15, &point) == STURDY_OK && point.line == 1 && point.column == 3, "an offset maps to a line and column");
        uint64_t offset = 0;
        check(sturdy_document_point_to_offset(document, point, &offset) == STURDY_OK && offset == 15, "and back");
        check(sturdy_document_point_to_offset(document, {0, 500}, &offset) == STURDY_ERROR_OUT_OF_RANGE, "a column past the line is out of range");
        check(sturdy_document_offset_to_utf16(document, 15, &point) == STURDY_OK && point.column == 3, "the UTF-16 column agrees for ASCII");
        check(sturdy_document_utf16_to_offset(document, point, &offset) == STURDY_OK && offset == 15, "the UTF-16 column maps back");
        check(sturdy_document_clip_offset(document, 99999, STURDY_FALSE, &offset) == STURDY_OK && offset == initial.size(), "clip snaps into the text");
        SturdyCharClass char_class{};
        check(sturdy_document_char_class_at(document, 1, &char_class) == STURDY_OK && char_class == STURDY_CHAR_CLASS_WORD, "a letter is a word character");
        check(sturdy_document_char_class_at(document, 99999, &char_class) == STURDY_ERROR_OUT_OF_RANGE, "an offset past the end is refused");
        check(sturdy_document_word_range_at(document, 2, &range) == STURDY_OK && range.start == 0 && range.end == 5, "the word around an offset");
        check(sturdy_document_next_word_boundary(document, 0, &offset) == STURDY_OK && offset > 0, "the next word boundary moves forward");
        check(sturdy_document_previous_word_boundary(document, 11, &offset) == STURDY_OK && offset < 11, "the previous word boundary moves back");
        check(sturdy_document_matching_bracket(document, 19, &offset) == STURDY_OK && offset == 24, "the bracket finds its partner");
        check(sturdy_document_matching_bracket(document, 1, &offset) == STURDY_ERROR_NOT_AVAILABLE, "no bracket at a letter");

        // Search.
        check(sturdy_document_find(document, "world", 0, STURDY_TRUE, STURDY_TRUE, &range) == STURDY_OK && range.start == 6, "forward search finds the word");
        check(sturdy_document_find(document, "WORLD", 0, STURDY_TRUE, STURDY_TRUE, &range) == STURDY_ERROR_NOT_AVAILABLE, "case-sensitive search misses");
        check(sturdy_document_find(document, "WORLD", 0, STURDY_TRUE, STURDY_FALSE, &range) == STURDY_OK, "case-insensitive search hits");
        SturdyByteRange all[4];
        uint32_t found = 0;
        check(sturdy_document_find_all(document, "l", STURDY_TRUE, all, 4, &found) == STURDY_OK && found == 4, "find_all counts matches");
        check(sturdy_document_find_all(document, "l", STURDY_TRUE, all, 2, &found) == STURDY_ERROR_BUFFER_TOO_SMALL && found == 4, "a small buffer reports the total");

        // Layout helpers and statistics.
        SturdyBool blank = STURDY_TRUE;
        check(sturdy_document_is_line_blank(document, 0, &blank) == STURDY_OK && !blank, "line 0 has text");
        check(sturdy_document_is_line_blank(document, 2, &blank) == STURDY_OK && blank, "the last line is blank");
        SturdyLineIndent indent{};
        check(sturdy_document_line_indent(document, 0, &indent) == STURDY_OK && indent.whitespace_bytes == 0, "no indent on line 0");
        SturdyIndentStyle style{};
        check(sturdy_document_detect_indent(document, &style) == STURDY_OK, "indent style is detected");
        SturdyLineEnding ending = STURDY_LINE_ENDING_CRLF;
        check(sturdy_document_detect_line_ending(document, &ending) == STURDY_OK && ending == STURDY_LINE_ENDING_LF, "LF line endings are detected");
        SturdyDocumentStats stats{};
        check(sturdy_document_stats(document, &stats) == STURDY_OK && stats.logical_bytes == initial.size() && stats.ascii, "stats describe the text");

        check(sturdy_document_destroy(document) == STURDY_OK, "the document is destroyed");
        check(sturdy_document_revision(document, &revision) != STURDY_OK, "a destroyed document no longer resolves");
        check(sturdy_document_destroy(document) != STURDY_OK, "destroying twice is refused");
    }

    void test_dock_panels() {
        SturdyDockWorkspace a{}, b{};
        check(sturdy_dock_create("a", &a) == STURDY_OK && sturdy_dock_create("b", &b) == STURDY_OK, "two workspaces are created");
        SturdyBool flag = STURDY_FALSE;
        check(sturdy_dock_is_empty(a, &flag) == STURDY_OK && flag, "a new workspace is empty");
        check(sturdy_dock_add_panel(a, "scene", "Scene", STURDY_TRUE, nullptr) == STURDY_OK, "a panel is added");
        check(sturdy_dock_add_panel(a, "scene", "Again", STURDY_TRUE, nullptr) == STURDY_ERROR_COMPONENT_PRESENT, "a duplicate id is refused");
        check(sturdy_dock_add_panel(a, "console", "Console", STURDY_FALSE, nullptr) == STURDY_OK, "a second panel is added");
        SturdyDockPlacement bad{STURDY_TRUE, 999999, STURDY_DOCK_ZONE_LEFT};
        check(sturdy_dock_add_panel(a, "ghost", "Ghost", STURDY_TRUE, &bad) == STURDY_ERROR_INVALID_ARGUMENT, "a placement on a missing node is refused");
        check(sturdy_dock_has_panel(a, "console", &flag) == STURDY_OK && flag, "the panel is there");
        SturdyDockPanelInfo info{};
        check(sturdy_dock_panel_info(a, "console", &info) == STURDY_OK && !info.closable && info.title_length == 7, "the panel's info reads back");
        char title[16] = {};
        check(sturdy_dock_panel_title(a, "console", title, sizeof(title), nullptr) == STURDY_OK && std::strcmp(title, "Console") == 0, "the title reads back");
        check(sturdy_dock_panel_info(a, "nope", &info) == STURDY_ERROR_NOT_AVAILABLE, "an unknown panel is not available");

        // Transfer between workspaces, and layout save/restore.
        check(sturdy_dock_transfer_panel(a, b, "console", nullptr) == STURDY_OK, "a panel moves between workspaces");
        check(sturdy_dock_has_panel(a, "console", &flag) == STURDY_OK && !flag && sturdy_dock_has_panel(b, "console", &flag) == STURDY_OK && flag, "it left one and joined the other");
        check(sturdy_dock_transfer_panel(a, b, "console", nullptr) == STURDY_ERROR_NOT_AVAILABLE, "transferring a panel the origin lacks reports it");
        check(sturdy_dock_transfer_panel(a, a, "scene", nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "transferring to itself is refused");
        size_t length = 0;
        check(sturdy_dock_save_layout(a, nullptr, 0, &length) == STURDY_OK && length > 1, "the layout can be saved");
        std::string layout(length, '\0');
        check(sturdy_dock_save_layout(a, layout.data(), layout.size(), &length) == STURDY_OK, "and read");
        layout.resize(length - 1);
        check(sturdy_dock_restore_layout(a, layout.c_str()) == STURDY_OK, "the saved layout restores");
        check(sturdy_dock_restore_layout(a, "garbage") == STURDY_ERROR_INVALID_ARGUMENT, "malformed layout text is refused");
        check(sturdy_dock_remove_panel(a, "scene") == STURDY_OK, "a panel is removed");
        check(sturdy_dock_is_empty(a, &flag) == STURDY_OK && flag, "the workspace is empty again");
        SturdyDockTabDrag drag{};
        check(sturdy_dock_active_tab_drag(b, &drag) == STURDY_ERROR_NOT_AVAILABLE, "no drag is in progress");
        uint32_t leaf = 0;
        (void)sturdy_dock_focused_leaf(b, &leaf);
        const float background[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        check(sturdy_dock_set_content_background(b, background) == STURDY_OK, "the content background can be set");

        // The frame calls need an open UI frame.
        check(sturdy_dock_begin_frame(b, 0, 0, 100, 100, 0.016f) == STURDY_ERROR_BUSY, "begin_frame outside a UI frame is refused");
        check(sturdy_dock_begin_panel_content(b, "console") == STURDY_ERROR_BUSY, "panel content outside a UI frame is refused");
        SturdyDockEvents events{};
        check(sturdy_dock_end_frame(b, &events) == STURDY_ERROR_BUSY, "end_frame outside a UI frame is refused");

        check(sturdy_dock_destroy(a) == STURDY_OK && sturdy_dock_destroy(b) == STURDY_OK, "the workspaces are destroyed");
        check(sturdy_dock_destroy(a) != STURDY_OK, "destroying twice is refused");
    }

    struct FrameState {
        SturdyDockWorkspace dock{};
        int builds = 0;
        SturdyResult begin = STURDY_ERROR_INTERNAL;
        SturdyResult panel_open = STURDY_ERROR_INTERNAL;
        SturdyResult hidden_panel = STURDY_OK;
        SturdyResult end = STURDY_ERROR_INTERNAL;
    };

    void test_dock_frame(SFT::Engine::Engine &engine_object, SturdyEngine engine) {
        FrameState state;
        check(sturdy_dock_create("frame", &state.dock) == STURDY_OK, "a workspace for the frame test");
        check(sturdy_dock_add_panel(state.dock, "main", "Main", STURDY_TRUE, nullptr) == STURDY_OK, "the first panel");
        check(sturdy_dock_add_panel(state.dock, "side", "Side", STURDY_TRUE, nullptr) == STURDY_OK, "the second panel is a tab of the same leaf");

        SturdyUiTreeDesc desc{};
        (void)sturdy_ui_tree_desc_init(&desc);
        desc.name = "dock";
        desc.target.kind = STURDY_UI_TREE_TARGET_HEADLESS;
        desc.target.headless_width = 640;
        desc.target.headless_height = 480;
        desc.user = &state;
        desc.builder = [](SturdyEngine e, SturdyUiTreeFrame *frame, void *user) {
            auto *s = static_cast<FrameState *>(user);
            ++s->builds;
            s->begin = sturdy_dock_begin_frame(s->dock, 0, 0, frame->width, frame->height, frame->delta_seconds);
            s->panel_open = sturdy_dock_begin_panel_content(s->dock, "side");
            if (s->panel_open == STURDY_OK) {
                (void)sturdy_ui_end_element(e);
            }
            // Only the active tab of a leaf has content this frame; the other reports itself hidden.
            const SturdyResult main_open = sturdy_dock_begin_panel_content(s->dock, "main");
            if (main_open == STURDY_OK) {
                (void)sturdy_ui_end_element(e);
            }
            s->hidden_panel = (s->panel_open == STURDY_OK && main_open == STURDY_OK) ? STURDY_ERROR_INTERNAL : STURDY_OK;
            SturdyDockEvents events{};
            s->end = sturdy_dock_end_frame(s->dock, &events);
        };
        SturdyEntity tree{};
        check(sturdy_ui_tree_spawn(engine, &desc, &tree) == STURDY_OK, "a headless tree hosts the dock frame");
        engine_object.update(1.0 / 60.0);
        check(state.builds == 1, "the tree built once");
        check(state.begin == STURDY_OK, "begin_frame works inside a tree builder");
        check(state.end == STURDY_OK, "end_frame works inside a tree builder");
        check(state.panel_open == STURDY_OK || state.panel_open == STURDY_ERROR_NOT_AVAILABLE, "panel content opens or reports a hidden tab");
        check(state.hidden_panel == STURDY_OK, "exactly one of two tabs in a leaf has content");
        SturdyUiTreeStats stats{};
        check(sturdy_ui_tree_stats(engine, tree, &stats) == STURDY_OK && stats.failed_builds == 0, "the build did not fail (the elements balanced)");
        check(sturdy_dock_destroy(state.dock) == STURDY_OK, "the frame test workspace is destroyed");
    }

} // namespace

int main() {
    using SFT::Ffi::HandleKind;
    using SFT::Ffi::ScopedHandle;

    test_document();
    test_dock_panels();

    SFT::Engine::Engine engine_object;
    const ScopedHandle engine_handle{HandleKind::Engine, &engine_object};
    test_dock_frame(engine_object, SturdyEngine{engine_handle.token()});

    SFT::Async::Scheduler::shutdown();
    if (failures == 0) {
        std::printf("EditorFfiTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
