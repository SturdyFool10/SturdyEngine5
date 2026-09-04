#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer {

    /// Reports whether `backend` requires each row of a buffer-to-texture copy to start on a
    /// 256-byte boundary.
    ///
    /// D3D12 enforces this natively; WebGPU rejects a copy that spans more than one row unless its
    /// pitch is a multiple of 256 too (see `WebGpuCommandEncoder.cpp`'s `texel_copy_layout`).
    /// Vulkan has no such requirement, so packing tightly there avoids the repack this function's
    /// sibling otherwise has to do.
    ///
    /// @param backend Backend the copy will run against.
    ///
    /// @return Returns `true` when rows must be padded before uploading.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool backend_requires_padded_texture_rows(RHI::BackendType backend) noexcept;

    /// Repacks a tightly packed mip chain so each level's rows start on a 256-byte boundary and
    /// each level itself starts on a 512-byte boundary.
    ///
    /// This is the same transform `Renderer::upload_texture_rgba` applies inline for its own
    /// callers; it is exposed here so a caller that stages its own upload buffer -- `TextureStreamer`
    /// is the other one that needs it -- produces bytes that `Renderer::submit_texture_upload`'s
    /// `padded_rows = true` path can read back out correctly. The two must stay in exact agreement:
    /// one packs from one end of the buffer, the other unpacks from the other.
    ///
    /// @param format Pixel format the data is encoded as.
    /// @param width Level-0 width.
    /// @param height Level-0 height.
    /// @param mip_levels Number of mip levels present in `tight_data`, tightly packed in order.
    /// @param tight_data The whole mip chain, tightly packed (one level immediately after another,
    ///        no alignment between them).
    ///
    /// @return Returns the repacked buffer, ready to hand to `RhiDevice::write_buffer` and then
    ///         `submit_texture_upload(..., padded_rows = true)`.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] vector<std::byte> pack_texture_upload_rows(RHI::Format format, u32 width, u32 height,
                                                             u32 mip_levels, span<const std::byte> tight_data);

} // namespace SFT::Renderer
