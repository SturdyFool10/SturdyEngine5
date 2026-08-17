#pragma once

#include <Foundation/src/Foundation.hpp>

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
using std::vector;

namespace SFT::Text {


    struct FontFaceInfo {
        UString family;
        UString subfamily;
        bool bold = false;
        bool italic = false;
        string file_path;
        unsigned int face_index = 0;
    };

    namespace Detail {

        /// Returns a human-readable name for the supplied read value.
        ///
        /// @param face `face` value used by the operation.
        /// @param name_id Identifier of the target object or resource.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] UString read_name(hb_face_t *face, hb_ot_name_id_t name_id);

        /// Reports whether ci holds.
        ///
        /// @param haystack `haystack` value used by the operation.
        /// @param needle `needle` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool contains_ci(const ustr &haystack, const ustr &needle) noexcept;

        /// Performs the equals ci operation using the supplied arguments.
        ///
        /// @param a `a` value used by the operation.
        /// @param b `b` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool equals_ci(const ustr &a, const ustr &b) noexcept;

        /// Reports whether font extension is available.
        ///
        /// @param path Filesystem path identifying the target resource.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool has_font_extension(const std::filesystem::path &path) noexcept;

    } // namespace Detail


    /// Performs the discover fonts operation using the supplied arguments.
    ///
    /// @param search_directories `search_directories` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<FontFaceInfo> discover_fonts(span<const string> search_directories);


    class FontDatabase {
      public:
        /// Constructs a `FontDatabase` in its default state.
        ///
        /// @note This function does not throw exceptions.
        FontDatabase() noexcept = default;
        /// Constructs a `FontDatabase` from the supplied initialization values.
        ///
        /// @param faces `faces` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit FontDatabase(vector<FontFaceInfo> faces);

        /// Creates a `FontDatabase` resource or value from the supplied parameters.
        ///
        /// @param search_directories `search_directories` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] static FontDatabase create(span<const string> search_directories);

        /// Returns the current or globally available faces value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] span<const FontFaceInfo> faces() const noexcept;


        /// Finds the requested entry in the available state.
        ///
        /// @param family `family` value used by the operation.
        /// @param bold `bold` value used by the operation.
        /// @param italic `italic` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<string> find(const ustr &family, bool bold = false, bool italic = false) const;

      private:
        vector<FontFaceInfo> faces_;
    };


    struct FontSettings {
        UString default_ui_font_family;
        UString default_world_font_family;
        UString emoji_font_family;
        vector<string> extra_search_directories;
    };

} // namespace SFT::Text
