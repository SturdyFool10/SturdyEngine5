#include <RHI/src/RHI/RayTracing.hpp>


namespace SFT::RHI {

    /// Sets the custom index and mask for this `RHI`.
    ///
    /// @param custom_index Zero-based index of the target element or entry.
    /// @param mask `mask` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void AccelerationStructureInstance::set_custom_index_and_mask(u32 custom_index, u8 mask) noexcept {
        custom_index_and_mask = (custom_index & 0x00ffffffu) | (static_cast<u32>(mask) << 24u);
    }

    /// Sets the shader binding table offset and flags for this `RHI`.
    ///
    /// @param offset Offset from the beginning of the relevant range or buffer.
    /// @param flags Flags controlling optional behavior.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void AccelerationStructureInstance::set_shader_binding_table_offset_and_flags(u32 offset, u8 flags) noexcept {
        shader_binding_table_offset_and_flags = (offset & 0x00ffffffu) | (static_cast<u32>(flags) << 24u);
    }

} // namespace SFT::RHI

