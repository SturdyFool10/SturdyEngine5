#include <Foundation/Foundation.hpp>
#include <graphicsPlatform/GraphicsPlatform.hpp>

namespace SFT::GraphicsPlatform {

    static_assert(sizeof(void *) == 8, "Linux/x86_64 graphicsPlatform requires a 64-bit target.");

} // namespace SFT::GraphicsPlatform
