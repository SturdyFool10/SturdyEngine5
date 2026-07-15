#include "FontDatabase.hpp"

namespace SFT::Text::Detail {

string read_name(hb_face_t *face, hb_ot_name_id_t name_id) {
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

bool contains_ci(string_view haystack, string_view needle) noexcept {
            auto it = std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return !it.empty();
        }

bool equals_ci(string_view a, string_view b) noexcept {
            return std::ranges::equal(a, b, [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

bool has_font_extension(const std::filesystem::path &path) noexcept {
            string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

vector<FontFaceInfo> discover_fonts(span<const string> search_directories) {
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

FontDatabase::FontDatabase(vector<FontFaceInfo> faces) : faces_(std::move(faces)) {}

[[nodiscard]] FontDatabase FontDatabase::create(span<const string> search_directories) {
            return FontDatabase(discover_fonts(search_directories));
        }

[[nodiscard]] span<const FontFaceInfo> FontDatabase::faces() const noexcept { return faces_; }

[[nodiscard]] optional<string> FontDatabase::find(string_view family, bool bold, bool italic) const {
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

} // namespace SFT::Text
