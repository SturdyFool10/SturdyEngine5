#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <cstdlib>
#include <string>
#include <vector>
#pragma endregion

#include <Platform/Fonts.hpp>

using std::string;
using std::vector;

namespace SFT::Platform {

    vector<string> font_search_directories() noexcept {
        vector<string> directories{
            "/usr/share/fonts",
            "/usr/local/share/fonts",
        };
        if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            directories.emplace_back(string(home) + "/.fonts");
            directories.emplace_back(string(home) + "/.local/share/fonts");
        }
        return directories;
    }

} // namespace SFT::Platform
