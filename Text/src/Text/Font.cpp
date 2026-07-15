#include "Font.hpp"

namespace SFT::Text {

Font::~Font() noexcept { reset(); }

Font::Font(Font &&other) noexcept
            : blob_(other.blob_), face_(other.face_), font_(other.font_) {
            other.blob_ = nullptr;
            other.face_ = nullptr;
            other.font_ = nullptr;
        }

Font &Font::operator=(Font &&other) noexcept {
            if (this != &other) {
                reset();
                blob_ = other.blob_;
                face_ = other.face_;
                font_ = other.font_;
                other.blob_ = nullptr;
                other.face_ = nullptr;
                other.font_ = nullptr;
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
            if (face_ != nullptr) {
                hb_face_destroy(face_);
            }
            if (blob_ != nullptr) {
                hb_blob_destroy(blob_);
            }
            font_ = nullptr;
            face_ = nullptr;
            blob_ = nullptr;
        }

[[nodiscard]] i32 Font::vertical_extent(hb_ot_metrics_tag_t tag) const noexcept {
            hb_position_t value = 0;
            if (font_ == nullptr || !hb_ot_metrics_get_position(font_, tag, &value)) {
                return 0;
            }
            return static_cast<i32>(value);
        }

} // namespace SFT::Text
