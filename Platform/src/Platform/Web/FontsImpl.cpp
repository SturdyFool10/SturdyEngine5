#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include <Platform/Fonts.hpp>

using std::string;
using std::vector;

namespace SFT::Platform {


    /// Returns the current or globally available font search directories value.
    ///
    /// @return Returns the current font search directories value.
    /// @note This function does not throw exceptions.
    vector<string> font_search_directories() noexcept {
        return {};
    }

} // namespace SFT::Platform
