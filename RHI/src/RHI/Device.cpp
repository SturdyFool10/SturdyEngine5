#include <RHI/Device.hpp>

namespace SFT::RHI {

FormatCapabilities portable_format_capabilities(Format format) noexcept {
    // The WebGPU core texture-format table: sampled/filterable/renderable/blendable/storage, and 1x + 4x multisampling.
    constexpr u32 one_and_four = 1u | 4u;
    switch (format) {
        case Format::RGBA8Unorm:
        case Format::RGBA16Float:
            return {.sampled = true, .filterable = true, .color_attachment = true, .blendable = true, .storage = true,
                    .sample_counts = one_and_four};
        case Format::RGBA8UnormSrgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8UnormSrgb:
        case Format::RGB10A2Unorm:
            return {.sampled = true, .filterable = true, .color_attachment = true, .blendable = true, .storage = false,
                    .sample_counts = one_and_four};
        case Format::RGBA32Float:
            // Filtering and blending of 32-bit floats are optional features in WebGPU.
            return {.sampled = true, .filterable = false, .color_attachment = true, .blendable = false, .storage = true,
                    .sample_counts = 1u};
        default:
            return {};
    }
}

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
