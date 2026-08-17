#include <RHI/src/RHI/RayTracing.hpp>


namespace SFT::RHI {

    void AccelerationStructureInstance::set_custom_index_and_mask(u32 custom_index, u8 mask) noexcept {
        custom_index_and_mask = (custom_index & 0x00ffffffu) | (static_cast<u32>(mask) << 24u);
    }

    void AccelerationStructureInstance::set_shader_binding_table_offset_and_flags(u32 offset, u8 flags) noexcept {
        shader_binding_table_offset_and_flags = (offset & 0x00ffffffu) | (static_cast<u32>(flags) << 24u);
    }

} // namespace SFT::RHI

