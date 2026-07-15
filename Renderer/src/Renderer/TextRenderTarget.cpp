#include "TextRenderTarget.hpp"

namespace SFT::Renderer {

[[nodiscard]] RHI::TextureHandle TextRenderTarget::texture() const noexcept { return texture_; }

[[nodiscard]] RHI::TextureViewHandle TextRenderTarget::view() const noexcept { return view_; }

[[nodiscard]] RHI::SamplerHandle TextRenderTarget::sampler() const noexcept { return sampler_; }

[[nodiscard]] u32 TextRenderTarget::width() const noexcept { return config_.width; }

[[nodiscard]] u32 TextRenderTarget::height() const noexcept { return config_.height; }

} // namespace SFT::Renderer
