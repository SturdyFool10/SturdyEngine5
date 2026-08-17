#include "TextCanvas.hpp"

namespace SFT::Renderer {

/// Returns the tile size for this `Renderer`.
///
/// @return Returns the current tile size value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 TextCanvas::tile_size() const noexcept { return tile_size_; }

} // namespace SFT::Renderer
