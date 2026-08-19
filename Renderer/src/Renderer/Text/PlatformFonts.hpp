#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

using std::string;
using std::vector;

namespace SFT::Text {


    /// Returns the current or globally available font search directories value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] vector<string> font_search_directories() noexcept;

} // namespace SFT::Text
