#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

using std::span;
using std::string_view;

namespace SFT::Core {


    struct ThirdPartyLicense {
        string_view project;
        string_view license_file_name;
        Foundation::EmbeddedText text;
    };


    /// Returns the current or globally available third party licenses value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    [[nodiscard]] span<const ThirdPartyLicense> third_party_licenses() noexcept;

} // namespace SFT::Core
