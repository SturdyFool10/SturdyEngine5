#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <hb-ot.h>
#include <hb.h>
#include <optional>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include "Error.hpp"
#include "Font.hpp"

using std::optional;
using std::span;
using std::string;
using std::vector;

namespace SFT::Text {

    // One discovered font face: its family/subfamily names (read from the font's own `name`
    // table — the authoritative source, not the filename) and where to load it from.
    // `italic`/`bold` are cheap heuristics off the subfamily string ("Italic"/"Oblique",
    // "Bold"/"Black"/"Heavy") — good enough for font-picking UI and FontDatabase::find(); a
    // precise weight number would need the `OS/2` table, which isn't reflected here yet.
    struct FontFaceInfo {
        UString family;
        UString subfamily;
        bool bold = false;
        bool italic = false;
        string file_path;
        unsigned int face_index = 0;
    };

    namespace Detail {

        [[nodiscard]] UString read_name(hb_face_t *face, hb_ot_name_id_t name_id);

        [[nodiscard]] bool contains_ci(const ustr &haystack, const ustr &needle) noexcept;

        [[nodiscard]] bool equals_ci(const ustr &a, const ustr &b) noexcept;

        [[nodiscard]] bool has_font_extension(const std::filesystem::path &path) noexcept;

    } // namespace Detail

    // Walks `search_directories` recursively for `.ttf`/`.otf`/`.ttc`/`.otc` files, opening each
    // candidate (via Font::load) and reading its family/subfamily out of the `name` table
    // (hb-ot-name.h — no FreeType, same as the rest of this package). A directory that doesn't
    // exist is skipped, not an error — callers typically pass OS-reported directories
    // (Platform::font_search_directories(), see Platform/Fonts.cppm) that may not all exist on
    // every machine. A file that fails to parse as a font is skipped, not fatal to the scan.
    [[nodiscard]] vector<FontFaceInfo> discover_fonts(span<const string> search_directories);

    // A queryable index over discovered faces, built once (discovery walks a lot of files — not
    // something to redo per lookup).
    class FontDatabase {
      public:
        FontDatabase() noexcept = default;
        explicit FontDatabase(vector<FontFaceInfo> faces);

        [[nodiscard]] static FontDatabase create(span<const string> search_directories);

        [[nodiscard]] span<const FontFaceInfo> faces() const noexcept;

        // Best-effort match: exact case-insensitive family name, closest bold/italic combination
        // (prefers an exact style match, then falls back to whatever that family has).
        [[nodiscard]] optional<string> find(const ustr &family, bool bold = false, bool italic = false) const;

      private:
        vector<FontFaceInfo> faces_;
    };

    // The font-configuration surface: which family serves each role, and where discovery should
    // additionally look beyond whatever the OS reports (Platform::font_search_directories()).
    // Building a FontDatabase from this is the caller's job (typically Renderer/Engine, which
    // already depends on both Platform and Text) — see FontDatabase::create(), which just wants a
    // flat directory list, so a caller merges `extra_search_directories` with
    // Platform::font_search_directories() and passes the combined span in.
    struct FontSettings {
        UString default_ui_font_family;
        UString default_world_font_family;
        UString emoji_font_family;
        vector<string> extra_search_directories;
    };

} // namespace SFT::Text
