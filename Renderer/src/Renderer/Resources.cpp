#include "Resources.hpp"

namespace SFT::Renderer {

[[nodiscard]] RHI::BufferHandle MeshResource::vertex_buffer_handle() const noexcept { return vertex_buffer; }

[[nodiscard]] RHI::BufferHandle MeshResource::index_buffer_handle() const noexcept { return index_buffer; }

} // namespace SFT::Renderer
