#include <Renderer/Text/Font.hpp>

#include <hb-ft.h>

namespace SFT::Text {

/// Destroys the `Text` and releases resources owned by it.
///
/// @note This function does not throw exceptions.
Font::~Font() noexcept { reset(); }

/// Performs the font operation for `Text` using the supplied arguments.
///
/// @param other Other object used by the operation.
///
/// @note This function does not throw exceptions.
Font::Font(Font &&other) noexcept
            : blob_(other.blob_), face_(other.face_), font_(other.font_), owns_face_(other.owns_face_),
              ft_library_(other.ft_library_), ft_face_(other.ft_face_), owned_bytes_(std::move(other.owned_bytes_)) {
            other.blob_ = nullptr;
            other.face_ = nullptr;
            other.font_ = nullptr;
            other.owns_face_ = false;
            other.ft_library_ = nullptr;
            other.ft_face_ = nullptr;
        }

/// Assigns a new value to this `Text`.
///
/// @param other Other object used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
Font &Font::operator=(Font &&other) noexcept {
            if (this != &other) {
                reset();
                blob_ = other.blob_;
                face_ = other.face_;
                font_ = other.font_;
                owns_face_ = other.owns_face_;
                ft_library_ = other.ft_library_;
                ft_face_ = other.ft_face_;
                owned_bytes_ = std::move(other.owned_bytes_);
                other.blob_ = nullptr;
                other.face_ = nullptr;
                other.font_ = nullptr;
                other.owns_face_ = false;
                other.ft_library_ = nullptr;
                other.ft_face_ = nullptr;
            }
            return *this;
        }

/// Loads the requested data or resource.
///
/// @param data Data consumed or referenced by the operation.
/// @param face_index Zero-based index of the target element or entry.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`, `TextErrorCode::LoadFailed`.
[[nodiscard]] TextExpected<Font> Font::load(span<const std::byte> data, unsigned int face_index) {
            if (data.empty()) {
                return text_error(TextErrorCode::InvalidArgument, "Cannot load a font from empty data.");
            }

            hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(data.data()),
                                             static_cast<unsigned int>(data.size()),
                                             HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
            if (blob == nullptr || blob == hb_blob_get_empty()) {
                if (blob != nullptr) {
                    hb_blob_destroy(blob);
                }
                return text_error(TextErrorCode::LoadFailed, "Failed to create a HarfBuzz blob from font data.");
            }

            hb_face_t *face = hb_face_create(blob, face_index);
            if (face == nullptr || face == hb_face_get_empty()) {
                if (face != nullptr) {
                    hb_face_destroy(face);
                }
                hb_blob_destroy(blob);
                return text_error(TextErrorCode::LoadFailed, "Failed to parse a font face from the given data.");
            }

            hb_font_t *font = hb_font_create(face);
            if (font == nullptr || font == hb_font_get_empty()) {
                if (font != nullptr) {
                    hb_font_destroy(font);
                }
                hb_face_destroy(face);
                hb_blob_destroy(blob);
                return text_error(TextErrorCode::LoadFailed, "Failed to create a HarfBuzz font from the parsed face.");
            }

            Font result;
            result.blob_ = blob;
            result.face_ = face;
            result.font_ = font;
            result.owns_face_ = true;
            return result;
        }

/// Loads hinted.
///
/// @param data Data consumed or referenced by the operation.
/// @param pixel_size Requested or available size for the operation.
/// @param face_index Zero-based index of the target element or entry.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`, `TextErrorCode::LoadFailed`.
[[nodiscard]] TextExpected<Font> Font::load_hinted(span<const std::byte> data, f32 pixel_size, unsigned int face_index) {
            if (data.empty()) {
                return text_error(TextErrorCode::InvalidArgument, "Cannot load a font from empty data.");
            }
            if (pixel_size <= 0.0f) {
                return text_error(TextErrorCode::InvalidArgument, "Cannot load a hinted font at a non-positive pixel size.");
            }

            FT_Library library = nullptr;
            if (FT_Init_FreeType(&library) != 0 || library == nullptr) {
                return text_error(TextErrorCode::LoadFailed, "Failed to initialize FreeType.");
            }

            Font result;
            result.owned_bytes_.assign(data.begin(), data.end());

            FT_Face face = nullptr;
            const FT_Error face_error = FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(result.owned_bytes_.data()),
                                                            static_cast<FT_Long>(result.owned_bytes_.size()),
                                                            static_cast<FT_Long>(face_index), &face);
            if (face_error != 0 || face == nullptr) {
                FT_Done_FreeType(library);
                return text_error(TextErrorCode::LoadFailed, "Failed to parse a font face from the given data via FreeType.");
            }

            if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size + 0.5f)) != 0) {
                FT_Done_Face(face);
                FT_Done_FreeType(library);
                return text_error(TextErrorCode::LoadFailed, "Failed to set the hinted font's pixel size.");
            }


            hb_font_t *font = hb_ft_font_create_referenced(face);
            if (font == nullptr || font == hb_font_get_empty()) {
                if (font != nullptr) {
                    hb_font_destroy(font);
                }
                FT_Done_Face(face);
                FT_Done_FreeType(library);
                return text_error(TextErrorCode::LoadFailed, "Failed to create a HarfBuzz font from the hinted FreeType face.");
            }


            hb_ft_font_set_load_flags(font, FT_LOAD_TARGET_NORMAL);


            const unsigned int upem = hb_face_get_upem(hb_font_get_face(font));
            hb_font_set_scale(font, static_cast<int>(upem), static_cast<int>(upem));

            result.font_ = font;
            result.face_ = hb_font_get_face(font);
            result.owns_face_ = false;
            result.ft_library_ = library;
            result.ft_face_ = face;
            return result;
        }

/// Returns the current or globally available valid value.
///
/// @return Returns the current valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool Font::valid() const noexcept { return font_ != nullptr; }

/// Converts the `Text` to `bool`.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] Font::operator bool() const noexcept { return valid(); }

/// Returns the current or globally available units per em value.
///
/// @return Returns the current units per em value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 Font::units_per_em() const noexcept {
            return face_ != nullptr ? static_cast<u32>(hb_face_get_upem(face_)) : 0;
        }

/// Returns the current or globally available ascender value.
///
/// @return Returns the current ascender value.
/// @note This function does not throw exceptions.
[[nodiscard]] i32 Font::ascender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER); }

/// Returns the current or globally available descender value.
///
/// @return Returns the current descender value.
/// @note This function does not throw exceptions.
[[nodiscard]] i32 Font::descender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER); }

/// Returns the current or globally available line gap value.
///
/// @return Returns the current line gap value.
/// @note This function does not throw exceptions.
[[nodiscard]] i32 Font::line_gap() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP); }

/// Returns the current or globally available handle value.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] hb_font_t *Font::handle() const noexcept { return font_; }

/// Returns the face handle associated with this `Text`.
///
/// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
/// @note This function does not throw exceptions.
[[nodiscard]] hb_face_t *Font::face_handle() const noexcept { return face_; }

/// Resets the object to its baseline state.
///
/// @return Returns the current reset value.
/// @note This function does not throw exceptions.
void Font::reset() noexcept {
            if (font_ != nullptr) {
                hb_font_destroy(font_);
            }
            if (face_ != nullptr && owns_face_) {
                hb_face_destroy(face_);
            }
            if (blob_ != nullptr) {
                hb_blob_destroy(blob_);
            }
            if (ft_face_ != nullptr) {
                FT_Done_Face(ft_face_);
            }
            if (ft_library_ != nullptr) {
                FT_Done_FreeType(ft_library_);
            }
            font_ = nullptr;
            face_ = nullptr;
            owns_face_ = false;
            blob_ = nullptr;
            ft_face_ = nullptr;
            ft_library_ = nullptr;
            owned_bytes_.clear();
        }

/// Performs the vertical extent operation for `Text` using the supplied arguments.
///
/// @param tag `tag` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] i32 Font::vertical_extent(hb_ot_metrics_tag_t tag) const noexcept {
            hb_position_t value = 0;
            if (font_ == nullptr || !hb_ot_metrics_get_position(font_, tag, &value)) {
                return 0;
            }
            return static_cast<i32>(value);
        }

} // namespace SFT::Text
