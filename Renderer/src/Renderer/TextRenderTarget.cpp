#include "TextRenderTarget.hpp"

namespace SFT::Renderer {

/// Returns the current or globally available texture value.
///
/// @return Returns the current texture value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::TextureHandle TextRenderTarget::texture() const noexcept { return texture_; }

/// Returns the current or globally available view value.
///
/// @return Returns the current view value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::TextureViewHandle TextRenderTarget::view() const noexcept { return view_; }

/// Returns the current or globally available sampler value.
///
/// @return Returns the current sampler value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::SamplerHandle TextRenderTarget::sampler() const noexcept { return sampler_; }

/// Returns the current or globally available width value.
///
/// @return Returns the current width value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 TextRenderTarget::width() const noexcept { return config_.width; }

/// Returns the current or globally available height value.
///
/// @return Returns the current height value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 TextRenderTarget::height() const noexcept { return config_.height; }

} // namespace SFT::Renderer
