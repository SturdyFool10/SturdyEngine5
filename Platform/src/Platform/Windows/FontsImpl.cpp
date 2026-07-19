#include <Foundation/src/Foundation.hpp>

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
        vector<string> directories;
        if (const char *windir = std::getenv("WINDIR"); windir != nullptr && windir[0] != '\0') {
            directories.emplace_back(string(windir) + "\\Fonts");
        } else {
            directories.emplace_back("C:\\Windows\\Fonts");
        }
        if (const char *local_app_data = std::getenv("LOCALAPPDATA"); local_app_data != nullptr && local_app_data[0] != '\0') {
            directories.emplace_back(string(local_app_data) + "\\Microsoft\\Windows\\Fonts");
        }
        return directories;
    }

} // namespace SFT::Platform
