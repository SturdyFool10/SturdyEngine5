#include "Font.hpp"

#include <hb-ft.h>

namespace SFT::Text {

Font::~Font() noexcept { reset(); }

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

            // _referenced: takes its own FT_Reference_Face on `face` and arranges to FT_Done_Face it
            // when this hb_font_t is destroyed — independent of the reference `face` above already
            // holds, which this Font still owns and releases itself in reset().
            hb_font_t *font = hb_ft_font_create_referenced(face);
            if (font == nullptr || font == hb_font_get_empty()) {
                if (font != nullptr) {
                    hb_font_destroy(font);
                }
                FT_Done_Face(face);
                FT_Done_FreeType(library);
                return text_error(TextErrorCode::LoadFailed, "Failed to create a HarfBuzz font from the hinted FreeType face.");
            }
            // Bakes in hinting at the exact ppem FT_Set_Pixel_Sizes was just called with (hinting is
            // a geometric change to FT_Outline point positions, decided once at FT_Load_Glyph time —
            // it survives whatever coordinate space the outline is later reported in).
            hb_ft_font_set_load_flags(font, FT_LOAD_TARGET_NORMAL);
            // hb-ft's default font scale is the active ppem in 26.6 fixed point (pixel space) —
            // override it to units_per_em so a hinted Font's Text::glyph_outline output lands in
            // the same font-design-unit coordinate space load()'s does, matching this package's
            // documented contract (Font::units_per_em()'s doc comment) regardless of loader.
            const unsigned int upem = hb_face_get_upem(hb_font_get_face(font));
            hb_font_set_scale(font, static_cast<int>(upem), static_cast<int>(upem));

            result.font_ = font;
            result.face_ = hb_font_get_face(font); // borrowed — owned by `font`, see owns_face_ below
            result.owns_face_ = false;
            result.ft_library_ = library;
            result.ft_face_ = face;
            return result;
        }

[[nodiscard]] bool Font::valid() const noexcept { return font_ != nullptr; }

[[nodiscard]] Font::operator bool() const noexcept { return valid(); }

[[nodiscard]] u32 Font::units_per_em() const noexcept {
            return face_ != nullptr ? static_cast<u32>(hb_face_get_upem(face_)) : 0;
        }

[[nodiscard]] i32 Font::ascender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER); }

[[nodiscard]] i32 Font::descender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER); }

[[nodiscard]] i32 Font::line_gap() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP); }

[[nodiscard]] hb_font_t *Font::handle() const noexcept { return font_; }

[[nodiscard]] hb_face_t *Font::face_handle() const noexcept { return face_; }

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

[[nodiscard]] i32 Font::vertical_extent(hb_ot_metrics_tag_t tag) const noexcept {
            hb_position_t value = 0;
            if (font_ == nullptr || !hb_ot_metrics_get_position(font_, tag, &value)) {
                return 0;
            }
            return static_cast<i32>(value);
        }

} // namespace SFT::Text
