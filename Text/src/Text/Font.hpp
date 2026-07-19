#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <hb-ot.h>
#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include "Error.hpp"

using std::span;
using std::vector;

namespace SFT::Text {

    // Owns a loaded font face and the HarfBuzz `hb_font_t` shaping/outline handle built from it.
    // Font bytes are copied into HarfBuzz's own buffer at load time (`HB_MEMORY_MODE_DUPLICATE`),
    // so the caller's source data need not outlive this object. A Font built via load_hinted()
    // additionally owns a FreeType FT_Library/FT_Face pair (see that function's doc comment).
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
        // collection; 0 for an ordinary single-face font file. Outlines extracted from a Font
        // loaded this way (Text::glyph_outline) are scale-free — safe to rasterize/reuse at any
        // pixel size, since nothing has been grid-fit to a specific size.
        [[nodiscard]] static TextExpected<Font> load(span<const std::byte> data, unsigned int face_index = 0);

        // Loads a font through FreeType instead of directly through HarfBuzz's hb-ot backend, with
        // FreeType's hinting engine (TrueType bytecode interpreter / CFF hinter) active at exactly
        // `pixel_size`. `data` is copied (FreeType's FT_New_Memory_Face does not own the buffer it's
        // given), so it need not outlive this call.
        //
        // Unlike load(), the resulting Font is tied to `pixel_size`: Text::glyph_outline on a
        // hinted Font returns an outline already grid-fit for that one exact size — rasterizing it
        // at any other size defeats the purpose of hinting (and looks wrong, the way a hinted
        // outline stretched 2x always does in any renderer). Only use this for a caller that always
        // draws at one fixed, known pixel size (a UI/HUD element, never scaled/rotated/animated);
        // TextAtlas::GlyphKey already caches per `reference_ppem`, so this composes with the atlas
        // as-is — a hinted glyph and an unhinted glyph at the same nominal size simply land in two
        // different cache entries.
        //
        // Coordinates still come out in font design units (hb_font_set_scale is used internally to
        // normalize hb-ft's default pixel-space output back to units_per_em), matching load()'s
        // contract exactly — a caller's existing `pixel_size / units_per_em` scale math is unchanged
        // by which loader produced the Font.
        [[nodiscard]] static TextExpected<Font> load_hinted(span<const std::byte> data, f32 pixel_size,
                                                             unsigned int face_index = 0);

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
        // load()'s face_ is a reference this Font created itself (hb_face_create) and must destroy;
        // load_hinted()'s face_ is only a borrowed peek (hb_font_get_face) into what font_ (via
        // hb_ft_font_create_referenced) already owns — destroying it too would double-free.
        bool owns_face_ = false;

        // Only set by load_hinted(); reset() tears these down (after font_, which holds its own
        // extra FT_Face reference via hb_ft_font_create_referenced) whenever they're non-null.
        FT_Library ft_library_ = nullptr;
        FT_Face ft_face_ = nullptr;
        vector<std::byte> owned_bytes_;
    };

} // namespace SFT::Text
