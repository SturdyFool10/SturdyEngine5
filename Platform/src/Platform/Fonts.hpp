#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

using std::string;
using std::vector;

namespace SFT::Platform {

    // The directories this OS keeps installed fonts in, in priority order (system-wide first,
    // per-user overrides last so a later duplicate family/style wins the way the OS itself
    // resolves it). Pure path data — no filesystem walking, no font parsing; that's
    // Text::discover_fonts()'s job (Text/FontDatabase.cppm), which already depends on HarfBuzz to
    // read the actual family/style out of each file. Kept here (not in Text) only because knowing
    // *where* an OS puts fonts is Platform's kind of fact, and Text is built before Platform in
    // the package dependency order (see root CMakeLists.txt) so it can't ask Platform directly.
    // A directory that doesn't exist on this machine is still returned — callers should skip
    // missing paths rather than treat an empty scan as an error.
    [[nodiscard]] vector<string> font_search_directories() noexcept;

} // namespace SFT::Platform
