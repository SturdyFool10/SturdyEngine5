#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include <Platform/Fonts.hpp>

using std::string;
using std::vector;

namespace SFT::Platform {

    // A browser sandbox has no filesystem font directories to scan — Web builds supply fonts as
    // bundled assets loaded through Text::Font::load() directly instead of OS discovery.
    vector<string> font_search_directories() noexcept {
        return {};
    }

} // namespace SFT::Platform
