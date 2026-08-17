#include "FontDatabase.hpp"

namespace SFT::Text::Detail {

/// Returns a human-readable name for the supplied read value.
///
/// @param face `face` value used by the operation.
/// @param name_id Identifier of the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
UString read_name(hb_face_t *face, hb_ot_name_id_t name_id) {


            unsigned int probe_capacity = 0;
            const unsigned int full_length = hb_ot_name_get_utf8(face, name_id, HB_LANGUAGE_INVALID, &probe_capacity, nullptr);
            if (full_length == 0) {
                return {};
            }
            string text(full_length, '\0');
            unsigned int capacity = full_length + 1;
            text.resize(capacity);
            hb_ot_name_get_utf8(face, name_id, HB_LANGUAGE_INVALID, &capacity, text.data());
            text.resize(std::min(capacity, full_length));
            return UString{text};
        }

/// Reports whether ci holds for this `Detail`.
///
/// @param haystack `haystack` value used by the operation.
/// @param needle `needle` value used by the operation.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
bool contains_ci(const ustr &haystack, const ustr &needle) noexcept {
            auto it = std::ranges::search(haystack.cpp_string_view(), needle.cpp_string_view(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return !it.empty();
        }

/// Performs the equals ci operation for `Detail` using the supplied arguments.
///
/// @param a `a` value used by the operation.
/// @param b `b` value used by the operation.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
bool equals_ci(const ustr &a, const ustr &b) noexcept {
            return std::ranges::equal(a.cpp_string_view(), b.cpp_string_view(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

/// Reports whether this `Detail` has font extension.
///
/// @param path Filesystem path identifying the target resource.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
bool has_font_extension(const std::filesystem::path &path) noexcept {
            string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc";
        }

} // namespace SFT::Text::Detail

namespace SFT::Text {

/// Performs the discover fonts operation for `Text` using the supplied arguments.
///
/// @param search_directories `search_directories` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

                UString family = Detail::read_name(font->face_handle(), HB_OT_NAME_ID_FONT_FAMILY);
                if (family.empty()) {
                    continue;
                }
                UString subfamily = Detail::read_name(font->face_handle(), HB_OT_NAME_ID_FONT_SUBFAMILY);
                const bool bold = Detail::contains_ci(subfamily.as_ustr(), "Bold"_ustr) ||
                                  Detail::contains_ci(subfamily.as_ustr(), "Black"_ustr) ||
                                  Detail::contains_ci(subfamily.as_ustr(), "Heavy"_ustr);
                const bool italic = Detail::contains_ci(subfamily.as_ustr(), "Italic"_ustr) ||
                                    Detail::contains_ci(subfamily.as_ustr(), "Oblique"_ustr);

                faces.push_back(FontFaceInfo{
                    .family = std::move(family),
                    .subfamily = std::move(subfamily),
                    .bold = bold,
                    .italic = italic,
                    .file_path = entry.path().string(),
                    .face_index = 0,
                });
            }
        }
        return faces;
    }

/// Performs the font database operation for `Text` using the supplied arguments.
///
/// @param faces `faces` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
FontDatabase::FontDatabase(vector<FontFaceInfo> faces) : faces_(std::move(faces)) {}

/// Creates a `Text` resource or value from the supplied parameters.
///
/// @param search_directories `search_directories` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] FontDatabase FontDatabase::create(span<const string> search_directories) {
            return FontDatabase(discover_fonts(search_directories));
        }

/// Returns the current or globally available faces value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const FontFaceInfo> FontDatabase::faces() const noexcept { return faces_; }

/// Finds the requested entry in the available state.
///
/// @param family `family` value used by the operation.
/// @param bold `bold` value used by the operation.
/// @param italic `italic` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<string> FontDatabase::find(const ustr &family, bool bold, bool italic) const {
            const FontFaceInfo *best = nullptr;
            int best_score = -1;
            for (const FontFaceInfo &face : faces_) {
                if (!Detail::equals_ci(face.family.as_ustr(), family)) {
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
