#include <D3D12/src/D3D12/D3D12Device.hpp>


namespace SFT::D3D12 {

    /// Reports whether composition present holds for this `D3D12`.
    ///
    /// @return Returns the current is composition present value.
    /// @note This function does not throw exceptions.
    bool SwapchainRecord::is_composition_present() const noexcept { return composition_target != nullptr; }

    /// Finds buffer for build in the available state.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const BufferRecord *D3D12Device::find_buffer_for_build(rhi::BufferHandle handle) const noexcept {
        return buffers_.find(handle);
    }

} // namespace SFT::D3D12

