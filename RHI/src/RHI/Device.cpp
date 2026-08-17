#include "Device.hpp"

namespace SFT::RHI {

/// Reports whether enabled holds for this `RHI`.
///
/// @param feature `feature` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool RhiDevice::is_enabled(Feature feature) const noexcept {
            return enabled_features().has(feature);
        }

/// Reports whether extension enabled holds for this `RHI`.
///
/// @param extension `extension` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool RhiDevice::is_extension_enabled(ExtensionId extension) const noexcept {
            return contains_extension(enabled_extensions(), extension);
        }

/// Creates a command encoder from the supplied parameters.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] RhiExpected<unique_ptr<CommandEncoder>> RhiDevice::create_command_encoder() {
            return create_command_encoder(CommandEncoderDesc{});
        }

/// Submits the requested work.
///
/// @param command_buffers Buffer used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
[[nodiscard]] RhiResult RhiDevice::submit(span<const CommandBufferHandle> command_buffers) {
            SubmitDesc desc;
            desc.command_buffers = command_buffers;
            return submit(desc);
        }

} // namespace SFT::RHI
