/// Covers the four places where WebGPU's resource rules are stricter than the RHI's own, each of
/// which Dawn rejects at creation or submission rather than at use:
///
///   * a host-visible buffer that is also a uniform or storage buffer, which cannot carry WebGPU's
///     MapWrite usage and so has to be mapped through a host shadow instead;
///   * a host-upload staging buffer written through the queue, which needs CopyDst to be written at
///     all;
///   * a buffer-to-texture copy, whose row pitch WebGPU wants in bytes and refuses to infer;
///   * a render target asking for a storage binding on a format WebGPU has no storage support for.
///
/// Every one of these reports through Dawn's uncaptured-error callback rather than through a return
/// value -- creation hands back a non-null object that is merely poisoned -- so the test watches the
/// log for what that callback writes rather than only checking the RHI's own results.

#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>

#include <Foundation/LogSink.hpp>
#include <RHI/RHI.hpp>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

    namespace rhi = SFT::RHI;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Collects everything Dawn's uncaptured-error callback logs, so a test can assert that a
    /// sequence of calls produced no validation error at all.
    ///
    /// The callback runs on whatever thread Dawn happens to be processing on, hence the mutex.
    class ValidationErrorWatch {
      public:
        /// Registers the log sink that collects validation errors.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        ValidationErrorWatch() {
            sink_ = SFT::Foundation::add_log_sink(
                [this](SFT::Foundation::LogLevel level, std::string_view message) {
                    if (level < SFT::Foundation::LogLevel::Error) {
                        return;
                    }
                    if (message.find("WebGPU validation error") == std::string_view::npos) {
                        return;
                    }
                    const std::lock_guard guard{mutex_};
                    messages_.emplace_back(message);
                });
        }

        /// Unregisters the log sink.
        ///
        /// @note This function does not throw exceptions.
        ~ValidationErrorWatch() { SFT::Foundation::remove_log_sink(sink_); }

        ValidationErrorWatch(const ValidationErrorWatch &) = delete;
        ValidationErrorWatch &operator=(const ValidationErrorWatch &) = delete;

        /// Reports whether anything was collected since the last `clear`, printing whatever was.
        ///
        /// @param what Name of the step being checked, for the failure message.
        ///
        /// @return Returns `true` when no validation error was seen.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] bool quiet(const char *what) {
            const std::lock_guard guard{mutex_};
            if (messages_.empty()) {
                return true;
            }
            std::cerr << "FAILED: " << what << " produced " << messages_.size()
                      << " WebGPU validation error(s):\n";
            for (const std::string &message : messages_) {
                std::cerr << "  " << message << '\n';
            }
            messages_.clear();
            return false;
        }

        /// Drops everything collected so far.
        ///
        /// @note This function does not throw exceptions.
        void clear() {
            const std::lock_guard guard{mutex_};
            messages_.clear();
        }

      private:
        SFT::Foundation::LogSinkId sink_{};
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };

    /// A host-visible buffer that is also a uniform and storage buffer -- the shape of the
    /// renderer's per-frame scene view buffer, and the one WebGPU's MapWrite exclusivity rules out.
    ///
    /// @param device Device under test.
    /// @param watch Validation-error collector.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool test_host_upload_shader_buffer(rhi::RhiDevice &device, ValidationErrorWatch &watch) {
        constexpr u64 size = 304;
        auto buffer = device.create_buffer(rhi::BufferDesc{
            .size = size,
            .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::Storage |
                     rhi::BufferUsage::TransferSrc,
            .memory = rhi::MemoryLocation::HostUpload,
            .label = "test host-upload shader buffer",
        });
        if (!check(buffer.has_value(), "a HostUpload uniform/storage buffer should be creatable")) {
            return false;
        }

        bool ok = true;
        auto mapped = device.map_buffer(*buffer);
        ok = check(mapped.has_value(), "a HostUpload buffer should be mappable") && ok;
        if (mapped) {
            ok = check(mapped->size() >= size, "the mapped range should cover the requested size") && ok;
            std::memset(mapped->data(), 0x5a, mapped->size());
            device.unmap_buffer(*buffer);
        }

        // Mapping a second time must show what the first map wrote: the RHI's contract is that the
        // buffer keeps its contents, and a shadow copy that started blank each time would not.
        auto remapped = device.map_buffer(*buffer);
        ok = check(remapped.has_value(), "a HostUpload buffer should be mappable twice") && ok;
        if (remapped && remapped->size() >= size) {
            ok = check(static_cast<unsigned char>((*remapped)[0]) == 0x5au &&
                           static_cast<unsigned char>((*remapped)[size - 1]) == 0x5au,
                       "a remapped HostUpload buffer should still hold what the last map wrote") &&
                 ok;
            device.unmap_buffer(*buffer);
        }

        device.wait_idle();
        ok = watch.quiet("mapping a HostUpload uniform/storage buffer") && ok;
        device.destroy_buffer(*buffer);
        return ok;
    }

    /// A staging buffer filled through the queue -- the shape of the renderer's texture staging
    /// buffer, which needs CopyDst for `write_buffer` to reach it.
    ///
    /// @param device Device under test.
    /// @param watch Validation-error collector.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool test_queue_written_staging_buffer(rhi::RhiDevice &device, ValidationErrorWatch &watch) {
        const std::vector<std::byte> contents(1024, std::byte{0x2b});
        auto buffer = device.create_buffer(rhi::BufferDesc{
            .size = contents.size(),
            .usage = rhi::BufferUsage::TransferSrc,
            .memory = rhi::MemoryLocation::HostUpload,
            .label = "test queue-written staging buffer",
        });
        if (!check(buffer.has_value(), "a HostUpload staging buffer should be creatable")) {
            return false;
        }

        bool ok = check(device.write_buffer(*buffer, 0, contents).has_value(),
                        "write_buffer should succeed on a HostUpload staging buffer");
        device.wait_idle();
        ok = watch.quiet("writing a HostUpload staging buffer through the queue") && ok;
        device.destroy_buffer(*buffer);
        return ok;
    }

    /// A buffer-to-texture copy of a multi-row mip 0, which is where a missing byte pitch shows up.
    ///
    /// Rows are padded to 256 bytes because WebGPU requires it of any copy spanning more than one
    /// row, exactly as D3D12 does; `Renderer::upload_texture_rgba` pads the same way.
    ///
    /// @param device Device under test.
    /// @param watch Validation-error collector.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool test_buffer_to_texture_copy(rhi::RhiDevice &device, ValidationErrorWatch &watch) {
        // 50 texels is deliberately not a multiple of 64, so the tight row (200 bytes) and the
        // padded row (256 bytes) differ and a pitch taken straight from the width would be wrong.
        constexpr u32 width = 50;
        constexpr u32 height = 8;
        constexpr u32 element_bytes = 4;
        constexpr u64 row_pitch = 256;

        auto texture = device.create_texture(rhi::TextureDesc{
            .dimension = rhi::TextureDimension::Dim2D,
            .format = rhi::Format::RGBA8Unorm,
            .extent = rhi::Extent3D{.width = width, .height = height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = rhi::SampleCount::X1,
            .usage = rhi::TextureUsage::TransferDst | rhi::TextureUsage::Sampled,
            .concurrent_queue_classes = {},
            .label = "test copy destination",
        });
        if (!check(texture.has_value(), "the copy destination texture should be creatable")) {
            return false;
        }

        const std::vector<std::byte> contents(row_pitch * height, std::byte{0x11});
        auto staging = device.create_buffer(rhi::BufferDesc{
            .size = contents.size(),
            .usage = rhi::BufferUsage::TransferSrc,
            .memory = rhi::MemoryLocation::HostUpload,
            .label = "test copy source",
        });
        if (!check(staging.has_value(), "the copy source buffer should be creatable")) {
            device.destroy_texture(*texture);
            return false;
        }

        bool ok = check(device.write_buffer(*staging, 0, contents).has_value(),
                        "the copy source buffer should be writable");

        auto encoder = device.create_command_encoder(
            rhi::CommandEncoderDesc{.label = "test copy encoder"});
        ok = check(encoder.has_value(), "a command encoder should be creatable") && ok;
        if (encoder) {
            const rhi::TextureBarrier to_transfer{
                .texture = *texture,
                .src_stage = rhi::PipelineStage::None,
                .src_access = rhi::AccessFlags::None,
                .dst_stage = rhi::PipelineStage::Transfer,
                .dst_access = rhi::AccessFlags::TransferWrite,
                .old_layout = rhi::TextureLayout::Undefined,
                .new_layout = rhi::TextureLayout::TransferDst,
            };
            (*encoder)->barrier({}, {}, std::span<const rhi::TextureBarrier>{&to_transfer, 1});
            (*encoder)->copy_buffer_to_texture(
                *staging, *texture,
                rhi::BufferTextureCopy{
                    .buffer_offset = 0,
                    // In texels, as the RHI counts it: the padded pitch divided by the texel size.
                    .buffer_row_length = static_cast<u32>(row_pitch / element_bytes),
                    .buffer_image_height = height,
                    .mip_level = 0,
                    .base_array_layer = 0,
                    .array_layer_count = 1,
                    .texture_offset = rhi::Offset3D{0, 0, 0},
                    .texture_extent =
                        rhi::Extent3D{.width = width, .height = height, .depth_or_layers = 1},
                });
            auto command_buffer = (*encoder)->finish();
            ok = check(command_buffer.has_value(), "the copy command buffer should finish") && ok;
            if (command_buffer) {
                const std::array buffers{*command_buffer};
                rhi::SubmitDesc submit_desc{};
                submit_desc.command_buffers =
                    std::span<const rhi::CommandBufferHandle>{buffers.data(), buffers.size()};
                submit_desc.label = "test copy submit";
                ok = check(device.submit(submit_desc).has_value(), "the copy should submit") && ok;
                device.wait_idle();
                device.destroy_command_buffer(*command_buffer);
            }
        }

        ok = watch.quiet("copying a buffer into a texture") && ok;
        device.destroy_buffer(*staging);
        device.destroy_texture(*texture);
        return ok;
    }

    /// A render target requesting a storage binding on RG16Float, which WebGPU has no storage
    /// support for -- the shape of the renderer's G-buffer normal and motion targets.
    ///
    /// @param device Device under test.
    /// @param watch Validation-error collector.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool test_storage_usage_on_unsupported_format(rhi::RhiDevice &device, ValidationErrorWatch &watch) {
        auto texture = device.create_texture(rhi::TextureDesc{
            .dimension = rhi::TextureDimension::Dim2D,
            .format = rhi::Format::RG16Float,
            .extent = rhi::Extent3D{.width = 64, .height = 64, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = rhi::SampleCount::X1,
            .usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled |
                     rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSrc,
            .concurrent_queue_classes = {},
            .label = "test rg16float storage-usage target",
        });
        bool ok = check(texture.has_value(),
                        "an RG16Float target asking for Storage should still be creatable");
        if (!texture) {
            return false;
        }

        auto view = device.create_texture_view(rhi::TextureViewDesc{
            .texture = *texture,
            .view_type = rhi::TextureViewType::View2D,
            .label = "test rg16float storage-usage target",
        });
        ok = check(view.has_value(), "a view of that target should be creatable") && ok;

        device.wait_idle();
        ok = watch.quiet("creating an RG16Float target that asked for a storage binding") && ok;
        if (view) {
            device.destroy_texture_view(*view);
        }
        device.destroy_texture(*texture);
        return ok;
    }

} // namespace

/// Entry point.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const rhi::BackendRegistration registration = SFT::Core::WebGpu::webgpu_backend_registration();
    auto instance = registration.create_instance(rhi::InstanceDesc{
        .application_name = "WebGpuResourceTest",
        .enable_validation = true,
        .enable_debug_utils = true,
    });
    if (!instance) {
        // No Dawn-capable device here -- a headless CI runner, or one with no Vulkan/Metal/D3D12
        // driver at all. There is nothing to test rather than something failing.
        std::cout << "SKIPPED: no WebGPU instance (" << instance.error().message << ")\n";
        return 0;
    }
    auto adapters = (*instance)->enumerate_adapters();
    if (!adapters || adapters->empty()) {
        std::cout << "SKIPPED: the WebGPU instance enumerated no adapters\n";
        return 0;
    }
    rhi::DeviceRequest device_request{};
    device_request.label = "test device";
    auto device = (*adapters->front()).create_device(device_request);
    if (!device) {
        std::cout << "SKIPPED: no WebGPU device (" << device.error().message << ")\n";
        return 0;
    }

    ValidationErrorWatch watch;
    // Device creation itself is not under test, and an adapter may well have logged something while
    // coming up.
    watch.clear();

    bool ok = true;
    ok = test_host_upload_shader_buffer(**device, watch) && ok;
    ok = test_queue_written_staging_buffer(**device, watch) && ok;
    ok = test_buffer_to_texture_copy(**device, watch) && ok;
    ok = test_storage_usage_on_unsupported_format(**device, watch) && ok;

    if (ok) {
        std::cout << "WebGpuResourceTest passed\n";
    }
    return ok ? 0 : 1;
}
