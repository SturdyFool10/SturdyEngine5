#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <hb-ot.h>
#include <hb.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <span>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/Text/Error.hpp>

using std::span;
using std::vector;

namespace SFT::Text {


    class Font {
      public:
        /// Constructs a `Font` in its default state.
        ///
        /// @note This function does not throw exceptions.
        Font() noexcept = default;
        /// Destroys the `Font` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~Font() noexcept;

        /// Disables this construction form for `Font`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Font(const Font &) = delete;
        /// Assigns a new value to this `Font`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        Font &operator=(const Font &) = delete;

        /// Constructs a `Font` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        Font(Font &&other) noexcept;

        /// Assigns a new value to this `Font`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        Font &operator=(Font &&other) noexcept;


        /// Loads the requested data or resource.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param face_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`, `TextErrorCode::LoadFailed`.
        [[nodiscard]] static TextExpected<Font> load(span<const std::byte> data, unsigned int face_index = 0);


        /// Loads hinted.
        ///
        /// @param data Data consumed or referenced by the operation.
        /// @param pixel_size Requested or available size for the operation.
        /// @param face_index Zero-based index of the target element or entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `TextErrorCode::InvalidArgument`, `TextErrorCode::LoadFailed`.
        [[nodiscard]] static TextExpected<Font> load_hinted(span<const std::byte> data, f32 pixel_size,
                                                             unsigned int face_index = 0);

        /// Returns the current or globally available valid value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool valid() const noexcept;
        /// Converts the `Font` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;


        /// Returns the current or globally available units per em value.
        ///
        /// @return Returns the current units per em value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 units_per_em() const noexcept;

        /// Returns the current or globally available ascender value.
        ///
        /// @return Returns the current ascender value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 ascender() const noexcept;
        /// Returns the current or globally available descender value.
        ///
        /// @return Returns the current descender value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 descender() const noexcept;
        /// Returns the current or globally available line gap value.
        ///
        /// @return Returns the current line gap value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 line_gap() const noexcept;


        /// Returns the current or globally available handle value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] hb_font_t *handle() const noexcept;
        /// Returns the face handle associated with this `Font`.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] hb_face_t *face_handle() const noexcept;

      private:
        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;

        /// Performs the vertical extent operation for `Font` using the supplied arguments.
        ///
        /// @param tag `tag` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] i32 vertical_extent(hb_ot_metrics_tag_t tag) const noexcept;

        hb_blob_t *blob_ = nullptr;
        hb_face_t *face_ = nullptr;
        hb_font_t *font_ = nullptr;


        bool owns_face_ = false;


        FT_Library ft_library_ = nullptr;
        FT_Face ft_face_ = nullptr;
        vector<std::byte> owned_bytes_;
    };

} // namespace SFT::Text
