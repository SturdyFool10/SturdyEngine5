#include <Renderer/Scene.hpp>


namespace SFT::Renderer {

    /// Converts the `Renderer` to `bool`.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    UiOverlayHooks::operator bool() const noexcept { return static_cast<bool>(prepare) && static_cast<bool>(draw); }

} // namespace SFT::Renderer

