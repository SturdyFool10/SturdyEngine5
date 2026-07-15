#include "Device.hpp"

namespace SFT::RHI {

[[nodiscard]] bool RhiDevice::is_enabled(Feature feature) const noexcept {
            return enabled_features().has(feature);
        }

[[nodiscard]] bool RhiDevice::is_extension_enabled(ExtensionId extension) const noexcept {
            return contains_extension(enabled_extensions(), extension);
        }

[[nodiscard]] RhiExpected<unique_ptr<CommandEncoder>> RhiDevice::create_command_encoder() {
            return create_command_encoder(CommandEncoderDesc{});
        }

[[nodiscard]] RhiResult RhiDevice::submit(span<const CommandBufferHandle> command_buffers) {
            SubmitDesc desc;
            desc.command_buffers = command_buffers;
            return submit(desc);
        }

} // namespace SFT::RHI
