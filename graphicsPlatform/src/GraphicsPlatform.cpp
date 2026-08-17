#include <graphicsPlatform/src/GraphicsPlatform.hpp>


namespace SFT::GraphicsPlatform {

    QueryMessage::operator bool() const noexcept { return status == QueryStatus::Ok; }

} // namespace SFT::GraphicsPlatform

