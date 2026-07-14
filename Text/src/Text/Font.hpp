#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <hb-ot.h>
#include <hb.h>
#include <span>
#include <utility>
#pragma endregion

#include "Error.hpp"

using std::span;

namespace SFT::Text {

    // Owns a loaded font face and the HarfBuzz `hb_font_t` shaping/outline handle built from it.
    // Font bytes are copied into HarfBuzz's own buffer at load time (`HB_MEMORY_MODE_DUPLICATE`),
    // so the caller's source data need not outlive this object.
    class Font {
      public:
        Font() noexcept = default;
        ~Font() noexcept { reset(); }

        Font(const Font &) = delete;
        Font &operator=(const Font &) = delete;

        Font(Font &&other) noexcept
            : blob_(other.blob_), face_(other.face_), font_(other.font_) {
            other.blob_ = nullptr;
            other.face_ = nullptr;
            other.font_ = nullptr;
        }

        Font &operator=(Font &&other) noexcept {
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

        // Loads a font (TrueType/OpenType/CFF/CFF2, including TTC/OTC collections) from raw bytes.
        // `data` is copied, so it need not outlive this call. `face_index` selects a face within a
        // collection; 0 for an ordinary single-face font file.
        [[nodiscard]] static TextExpected<Font> load(span<const std::byte> data, unsigned int face_index = 0) {
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

        [[nodiscard]] bool valid() const noexcept { return font_ != nullptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

        // The font's design grid (font units per em-square) — every shaping/outline coordinate
        // this package returns is in these units until a caller scales by the desired pixel size.
        [[nodiscard]] u32 units_per_em() const noexcept {
            return face_ != nullptr ? static_cast<u32>(hb_face_get_upem(face_)) : 0;
        }

        [[nodiscard]] i32 ascender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER); }
        [[nodiscard]] i32 descender() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER); }
        [[nodiscard]] i32 line_gap() const noexcept { return vertical_extent(HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP); }

        // Non-owning access for lower-level HarfBuzz calls (shaping, hb-draw outline extraction)
        // implemented in this module's other partitions.
        [[nodiscard]] hb_font_t *handle() const noexcept { return font_; }
        [[nodiscard]] hb_face_t *face_handle() const noexcept { return face_; }

      private:
        void reset() noexcept {
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

        [[nodiscard]] i32 vertical_extent(hb_ot_metrics_tag_t tag) const noexcept {
            hb_position_t value = 0;
            if (font_ == nullptr || !hb_ot_metrics_get_position(font_, tag, &value)) {
                return 0;
            }
            return static_cast<i32>(value);
        }

        hb_blob_t *blob_ = nullptr;
        hb_face_t *face_ = nullptr;
        hb_font_t *font_ = nullptr;
    };

} // namespace SFT::Text
