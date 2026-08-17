#include <Renderer/src/Renderer/Scene.hpp>


namespace SFT::Renderer {

    UiOverlayHooks::operator bool() const noexcept { return static_cast<bool>(prepare) && static_cast<bool>(draw); }

} // namespace SFT::Renderer

