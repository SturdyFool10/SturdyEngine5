#pragma once

#include <Foundation/Foundation.hpp>

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
using std::string_view;
using std::vector;

namespace SFT::Text {

    // One discovered font face: its family/subfamily names (read from the font's own `name`
    // table — the authoritative source, not the filename) and where to load it from.
    // `italic`/`bold` are cheap heuristics off the subfamily string ("Italic"/"Oblique",
    // "Bold"/"Black"/"Heavy") — good enough for font-picking UI and FontDatabase::find(); a
    // precise weight number would need the `OS/2` table, which isn't reflected here yet.
    struct FontFaceInfo {
        string family;
        string subfamily;
        bool bold = false;
        bool italic = false;
        string file_path;
        unsigned int face_index = 0;
    };

    namespace Detail {

        [[nodiscard]] inline string read_name(hb_face_t *face, hb_ot_name_id_t name_id) {
            // hb_ot_name_get_utf8 follows HarfBuzz's snprintf-style idiom: the return value is the
            // *full* string length regardless of truncation, while `text_size` is IN (buffer
            // capacity) / OUT (bytes actually written) — a zero-capacity probe call still reports
            // the true length via the return value, even though it writes nothing.
            unsigned int probe_capacity = 0;
            const unsigned int full_length = hb_ot_name_get_utf8(face, name_id, HB_LANGUAGE_INVALID, &probe_capacity, nullptr);
            if (full_length == 0) {
                return {};
            }
            string text(full_length, '\0');
            unsigned int capacity = full_length + 1; // + room for the trailing NUL hb writes
            text.resize(capacity);
            hb_ot_name_get_utf8(face, name_id, HB_LANGUAGE_INVALID, &capacity, text.data());
            text.resize(std::min(capacity, full_length));
            return text;
        }

        [[nodiscard]] inline bool contains_ci(string_view haystack, string_view needle) noexcept {
            auto it = std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return !it.empty();
        }

        [[nodiscard]] inline bool equals_ci(string_view a, string_view b) noexcept {
            return std::ranges::equal(a, b, [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

        [[nodiscard]] inline bool has_font_extension(const std::filesystem::path &path) noexcept {
            string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
        }

    } // namespace Detail

    // Walks `search_directories` recursively for `.ttf`/`.otf`/`.ttc`/`.otc` files, opening each
    // candidate (via Font::load) and reading its family/subfamily out of the `name` table
    // (hb-ot-name.h — no FreeType, same as the rest of this package). A directory that doesn't
    // exist is skipped, not an error — callers typically pass OS-reported directories
    // (Platform::font_search_directories(), see Platform/Fonts.cppm) that may not all exist on
    // every machine. A file that fails to parse as a font is skipped, not fatal to the scan.
    [[nodiscard]] inline vector<FontFaceInfo> discover_fonts(span<const string> search_directories) {
        vector<FontFaceInfo> faces;
        namespace fs = std::filesystem;

        for (const string &directory : search_directories) {
            std::error_code walk_error;
            fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, walk_error);
            if (walk_error) {
                continue;
            }
            for (const auto &entry : it) {
                if (!entry.is_regular_file() || !Detail::has_font_extension(entry.path())) {
                    continue;
                }

                std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
                if (!file) {
                    continue;
                }
                const std::streamsize size = file.tellg();
                if (size <= 0) {
                    continue;
                }
                file.seekg(0);
                vector<std::byte> bytes(static_cast<usize>(size));
                if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
                    continue;
                }

                auto font = Font::load(span<const std::byte>{bytes.data(), bytes.size()});
                if (!font) {
                    continue;
                }

                const string family = Detail::read_name(font->face_handle(), HB_OT_NAME_ID_FONT_FAMILY);
                if (family.empty()) {
                    continue;
                }
                const string subfamily = Detail::read_name(font->face_handle(), HB_OT_NAME_ID_FONT_SUBFAMILY);

                faces.push_back(FontFaceInfo{
                    .family = family,
                    .subfamily = subfamily,
                    .bold = Detail::contains_ci(subfamily, "Bold") || Detail::contains_ci(subfamily, "Black") ||
                            Detail::contains_ci(subfamily, "Heavy"),
                    .italic = Detail::contains_ci(subfamily, "Italic") || Detail::contains_ci(subfamily, "Oblique"),
                    .file_path = entry.path().string(),
                    .face_index = 0,
                });
            }
        }
        return faces;
    }

    // A queryable index over discovered faces, built once (discovery walks a lot of files — not
    // something to redo per lookup).
    class FontDatabase {
      public:
        FontDatabase() noexcept = default;
        explicit FontDatabase(vector<FontFaceInfo> faces) : faces_(std::move(faces)) {}

        [[nodiscard]] static FontDatabase create(span<const string> search_directories) {
            return FontDatabase(discover_fonts(search_directories));
        }

        [[nodiscard]] span<const FontFaceInfo> faces() const noexcept { return faces_; }

        // Best-effort match: exact case-insensitive family name, closest bold/italic combination
        // (prefers an exact style match, then falls back to whatever that family has).
        [[nodiscard]] optional<string> find(string_view family, bool bold = false, bool italic = false) const {
            const FontFaceInfo *best = nullptr;
            int best_score = -1;
            for (const FontFaceInfo &face : faces_) {
                if (!Detail::equals_ci(face.family, family)) {
                    continue;
                }
                const int score = (face.bold == bold ? 1 : 0) + (face.italic == italic ? 1 : 0);
                if (score > best_score) {
                    best_score = score;
                    best = &face;
                }
            }
            if (best == nullptr) {
                return std::nullopt;
            }
            return best->file_path;
        }

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
        string default_ui_font_family;
        string default_world_font_family;
        string emoji_font_family;
        vector<string> extra_search_directories;
    };

} // namespace SFT::Text
