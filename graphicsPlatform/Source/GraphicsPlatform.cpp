#include <Foundation/src/Foundation.hpp>
#include <graphicsPlatform/src/GraphicsPlatform.hpp>

namespace SFT::GraphicsPlatform {

    namespace {
        constexpr const char *notes[] = {
            "graphicsPlatform base library loaded",
        };
    }

    std::span<const char *const> compiled_backend_notes() noexcept {
        return notes;
    }

} // namespace SFT::GraphicsPlatform
