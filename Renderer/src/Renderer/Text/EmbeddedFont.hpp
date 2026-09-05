#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstddef>
#include <span>
#pragma endregion

namespace SFT::Text {

    /// Returns the bytes of the font embedded into the binary at compile time (see
    /// cmake/SturdyFonts.cmake), for use where no real font file can be found on disk -- notably
    /// Web, which has no filesystem to search at all (see Text/Platform/Web/FontsImpl.cpp's
    /// font_search_directories()), but usable as a last resort on any platform.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid for the
    ///         lifetime of the program (backed by static storage).
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::span<const std::byte> embedded_default_font_bytes() noexcept;

} // namespace SFT::Text
