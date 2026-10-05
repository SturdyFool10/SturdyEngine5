#pragma once

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Renderer {

    /// Creates a host-visible buffer holding `bytes` for this frame only: the handle is appended to `transient_buffers`
    /// (`FrameBuildContext::transient_buffers`), which the renderer frees once the frame has finished on the GPU. The way a
    /// feature hands per-frame constants to a compute kernel (`ComputeBufferBinding`) without owning any buffer lifetime.
    /// Buffers are at least 256 bytes so a small constant block still satisfies every backend's minimum binding size.
    [[nodiscard]] inline Core::RendererExpected<RHI::BufferHandle> upload_transient_buffer(
        RHI::RhiDevice &device, std::vector<RHI::BufferHandle> &transient_buffers, RHI::BufferUsage usage,
        std::span<const std::byte> bytes, const char *label) {
        auto buffer = device.create_buffer(RHI::BufferDesc{
            .size = std::max<u64>(bytes.size(), 256u), .usage = usage, .memory = RHI::MemoryLocation::HostUpload, .label = label});
        if (!buffer) {
            return std::unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                              std::string{"create "} + label + ": " + buffer.error().message});
        }
        transient_buffers.push_back(*buffer);
        if (!bytes.empty()) {
            if (auto written = device.write_buffer(*buffer, 0, bytes); !written) {
                return std::unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                                  std::string{"write "} + label + ": " + written.error().message});
            }
        }
        return *buffer;
    }

} // namespace SFT::Renderer
