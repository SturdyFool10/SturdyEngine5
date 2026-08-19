#include <Foundation/Foundation.hpp>
#include <Core/GraphicsPlatform/GraphicsPlatform.hpp>

namespace SFT::Core::GraphicsPlatform {

    static_assert(sizeof(void *) == 8, "Linux/x86_64 graphicsPlatform requires a 64-bit target.");

} // namespace SFT::Core::GraphicsPlatform
