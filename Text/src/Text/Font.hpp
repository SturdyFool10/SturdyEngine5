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
        ~Font() noexcept;

        Font(const Font &) = delete;
        Font &operator=(const Font &) = delete;

        Font(Font &&other) noexcept;

        Font &operator=(Font &&other) noexcept;

        // Loads a font (TrueType/OpenType/CFF/CFF2, including TTC/OTC collections) from raw bytes.
        // `data` is copied, so it need not outlive this call. `face_index` selects a face within a
        // collection; 0 for an ordinary single-face font file.
        [[nodiscard]] static TextExpected<Font> load(span<const std::byte> data, unsigned int face_index = 0);

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

        // The font's design grid (font units per em-square) — every shaping/outline coordinate
        // this package returns is in these units until a caller scales by the desired pixel size.
        [[nodiscard]] u32 units_per_em() const noexcept;

        [[nodiscard]] i32 ascender() const noexcept;
        [[nodiscard]] i32 descender() const noexcept;
        [[nodiscard]] i32 line_gap() const noexcept;

        // Non-owning access for lower-level HarfBuzz calls (shaping, hb-draw outline extraction)
        // implemented in this module's other partitions.
        [[nodiscard]] hb_font_t *handle() const noexcept;
        [[nodiscard]] hb_face_t *face_handle() const noexcept;

      private:
        void reset() noexcept;

        [[nodiscard]] i32 vertical_extent(hb_ot_metrics_tag_t tag) const noexcept;

        hb_blob_t *blob_ = nullptr;
        hb_face_t *face_ = nullptr;
        hb_font_t *font_ = nullptr;
    };

} // namespace SFT::Text
