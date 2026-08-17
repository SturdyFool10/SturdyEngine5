#include <graphicsPlatform/src/GraphicsPlatform.hpp>


namespace SFT::GraphicsPlatform {

    /// Converts the `GraphicsPlatform` to `bool`.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::Ok`.
    /// @note This function does not throw exceptions.
    QueryMessage::operator bool() const noexcept { return status == QueryStatus::Ok; }

} // namespace SFT::GraphicsPlatform

