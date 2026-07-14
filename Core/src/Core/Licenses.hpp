#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <string_view>
#pragma endregion

using std::span;
using std::string_view;

namespace SFT::Core {

    // One third-party dependency's license, embedded into the binary at compile time (see
    // cmake/SturdyLicenses.cmake and thirdparty/licenses/) so an application built on this
    // engine can render an in-app "Open Source Licenses" screen with zero runtime file I/O and
    // no dependency on thirdparty/licenses/ existing next to the shipped binary.
    struct ThirdPartyLicense {
        string_view project;            // dependency name, e.g. "box3d"
        string_view license_file_name;   // upstream file name, e.g. "LICENSE.md"
        Foundation::EmbeddedText text;   // the license's full text
    };

    // Every third-party dependency's license known at compile time — one entry per dependency
    // that was actually fetched for this build (see sturdy_configure_dependencies()). Regenerated
    // at configure time, so it always matches exactly the dependency set built into this binary,
    // regardless of whether STURDY_PREFER_SYSTEM_DEPENDENCIES caused any of them to resolve to a
    // system package rather than a vendored checkout.
    [[nodiscard]] span<const ThirdPartyLicense> third_party_licenses() noexcept;

} // namespace SFT::Core
