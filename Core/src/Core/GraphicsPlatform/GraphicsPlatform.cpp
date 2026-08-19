#include <Foundation/Foundation.hpp>
#include <Core/GraphicsPlatform/GraphicsPlatform.hpp>

namespace SFT::Core::GraphicsPlatform {

    namespace {
        constexpr const char *notes[] = {
            "graphicsPlatform base library loaded",
        };
    }

    /// Compiles the supplied source or pipeline state.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    std::span<const char *const> compiled_backend_notes() noexcept {
        return notes;
    }

} // namespace SFT::Core::GraphicsPlatform
