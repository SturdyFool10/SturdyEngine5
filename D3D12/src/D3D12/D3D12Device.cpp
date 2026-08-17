#include <D3D12/src/D3D12/D3D12Device.hpp>


namespace SFT::D3D12 {

    bool SwapchainRecord::is_composition_present() const noexcept { return composition_target != nullptr; }

    const BufferRecord *D3D12Device::find_buffer_for_build(rhi::BufferHandle handle) const noexcept {
        return buffers_.find(handle);
    }

} // namespace SFT::D3D12

