#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <utility>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    namespace {


        /// Computes the texture data bytes required by the supplied values.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 texture_data_bytes(RHI::Format format, u32 width, u32 height) noexcept {
            if (width == 0 || height == 0) {
                return 0;
            }
            if (RHI::format_is_block_compressed(format)) {
                u32 bytes_per_block = 0;
                switch (format) {
                    case RHI::Format::BC7Unorm:
                    case RHI::Format::BC7UnormSrgb:
                    case RHI::Format::BC5Unorm:
                    case RHI::Format::BC3Unorm:
                    case RHI::Format::BC3UnormSrgb: bytes_per_block = 16; break;
                    case RHI::Format::BC4Unorm:
                    case RHI::Format::BC1Unorm:
                    case RHI::Format::BC1UnormSrgb: bytes_per_block = 8; break;
                    default: return 0;
                }
                const u64 blocks_wide = (static_cast<u64>(width) + 3u) / 4u;
                const u64 blocks_high = (static_cast<u64>(height) + 3u) / 4u;
                if (blocks_wide > std::numeric_limits<u64>::max() / blocks_high) {
                    return 0;
                }
                const u64 block_count = blocks_wide * blocks_high;
                return block_count <= std::numeric_limits<u64>::max() / bytes_per_block
                    ? block_count * bytes_per_block
                    : 0;
            }

            u32 texel_size = 0;
            switch (format) {
                case RHI::Format::R8Unorm: texel_size = 1; break;
                case RHI::Format::RGBA8Unorm:
                case RHI::Format::RGBA8UnormSrgb: texel_size = 4; break;
                default: return 0;
            }
            const u64 texels = static_cast<u64>(width) * height;
            return texels <= std::numeric_limits<u64>::max() / texel_size ? texels * texel_size : 0;
        }

        /// Computes the texture copy offset alignment required by the supplied values.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 texture_copy_offset_alignment(RHI::Format format) noexcept {
            switch (format) {
                case RHI::Format::BC7Unorm:
                case RHI::Format::BC7UnormSrgb:
                case RHI::Format::BC5Unorm:
                case RHI::Format::BC3Unorm:
                case RHI::Format::BC3UnormSrgb: return 16;
                case RHI::Format::BC4Unorm:
                case RHI::Format::BC1Unorm:
                case RHI::Format::BC1UnormSrgb: return 8;
                case RHI::Format::R8Unorm:
                case RHI::Format::RGBA8Unorm:
                case RHI::Format::RGBA8UnormSrgb: return 4;
                default: return 0;
            }
        }

        /// Performs the align up operation for `Renderer` using the supplied arguments.
        ///
        /// @param value Value consumed by the operation.
        /// @param alignment `alignment` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 align_up(u64 value, u64 alignment) noexcept {
            if (alignment == 0 || value > std::numeric_limits<u64>::max() - (alignment - 1u)) {
                return 0;
            }
            return ((value + alignment - 1u) / alignment) * alignment;
        }

        /// Returns the texture mip level count for this `Renderer`.
        ///
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 texture_mip_level_count(u32 width, u32 height) noexcept {
            u32 levels = 0;
            while (width != 0 && height != 0) {
                ++levels;
                if (width == 1 && height == 1) {
                    break;
                }
                width = std::max(width / 2u, 1u);
                height = std::max(height / 2u, 1u);
            }
            return levels;
        }

        /// Computes the texture mip chain bytes required by the supplied values.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        /// @param width Width of the target extent.
        /// @param height Height of the target extent.
        /// @param mip_levels `mip_levels` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u64 texture_mip_chain_bytes(
            RHI::Format format, u32 width, u32 height, u32 mip_levels) noexcept {
            if (mip_levels == 0 || mip_levels > texture_mip_level_count(width, height)) {
                return 0;
            }
            const u64 alignment = texture_copy_offset_alignment(format);
            if (alignment == 0) {
                return 0;
            }
            u64 total = 0;
            for (u32 level = 0; level < mip_levels; ++level) {
                if (level != 0) {
                    total = align_up(total, alignment);
                    if (total == 0) {
                        return 0;
                    }
                }
                const u64 level_bytes = texture_data_bytes(format, width, height);
                if (level_bytes == 0 || total > std::numeric_limits<u64>::max() - level_bytes) {
                    return 0;
                }
                total += level_bytes;
                width = std::max(width / 2u, 1u);
                height = std::max(height / 2u, 1u);
            }
            return total;
        }

    } // namespace

    /// Creates a texture from the supplied parameters.
    ///
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param data Data consumed or referenced by the operation.
    /// @param label `label` value used by the operation.
    /// @param concurrent_queue_classes Queue used or affected by the operation.
    /// @param mip_levels `mip_levels` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<TextureHandle> Renderer::create_texture(u32 width, u32 height, RHI::Format format,
                                                                   span<const std::byte> data, const char *label,
                                                                   span<const RHI::QueueClass> concurrent_queue_classes,
                                                                   u32 mip_levels) {
        ZoneScopedN("Renderer::create_texture");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a texture without an RHI device."});
        }
        if (width == 0 || height == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a texture with a zero dimension."});
        }
        const u64 expected_bytes = texture_mip_chain_bytes(format, width, height, mip_levels);
        if (expected_bytes == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Unsupported texture format or invalid mip count for renderer create_texture."});
        }
        if (!data.empty() && static_cast<u64>(data.size()) != expected_bytes) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Texture pixel data size does not match the format's expected byte size."});
        }

        TextureResource resource{};
        resource.handle = TextureHandle{static_cast<u64>(textures_.size() + 1)};
        resource.label = label ? label : "";
        resource.width = width;
        resource.height = height;
        resource.mip_levels = mip_levels;
        resource.format = format;
        resource.pixel_data.assign(data.begin(), data.end());
        resource.concurrent_queue_classes.assign(concurrent_queue_classes.begin(), concurrent_queue_classes.end());
        resource.alive = true;

        if (Core::RendererResult created = create_owned_texture_gpu(resource, concurrent_queue_classes); !created.has_value()) {
            return unexpected(created.error());
        }
        textures_.push_back(std::move(resource));
        return textures_.back().handle;
    }

    /// Creates a owned texture GPU from the supplied parameters.
    ///
    /// @param resource `resource` value used by the operation.
    /// @param concurrent_queue_classes Queue used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::create_owned_texture_gpu(TextureResource &resource,
                                                            span<const RHI::QueueClass> concurrent_queue_classes) {
        ZoneScopedN("Renderer::create_owned_texture_gpu");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an owned texture without an RHI device.");
        }
        if (!resource.owns_gpu_resources || resource.width == 0 || resource.height == 0 || resource.mip_levels == 0 ||
            resource.format == RHI::Format::Undefined) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Owned texture is missing a valid CPU replay description.");
        }

        if (!concurrent_queue_classes.empty()) {
            resource.concurrent_queue_classes.assign(
                concurrent_queue_classes.begin(), concurrent_queue_classes.end());
        }
        const span<const RHI::QueueClass> replay_queue_classes{
            resource.concurrent_queue_classes.data(), resource.concurrent_queue_classes.size()};

        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = resource.format,
            .extent = RHI::Extent3D{.width = resource.width, .height = resource.height, .depth_or_layers = 1},
            .mip_levels = resource.mip_levels,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst,
            .concurrent_queue_classes = replay_queue_classes,
            .label = resource.label.empty() ? "renderer texture" : resource.label.c_str(),
        });
        if (!texture) {
            return unexpected(graphics_error_from_rhi(texture.error(), "create RHI texture"));
        }
        resource.texture = *texture;

        auto view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = resource.texture,
            .view_type = RHI::TextureViewType::View2D,
            .label = "renderer texture view",
        });
        if (!view) {
            device->destroy_texture(resource.texture);
            resource.texture = {};
            return unexpected(graphics_error_from_rhi(view.error(), "create RHI texture view"));
        }
        resource.view = *view;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .mipmap_mode = RHI::MipmapMode::Linear,
            .max_lod = static_cast<f32>(resource.mip_levels - 1u),
            .max_anisotropy = 8.0f,
            .label = "renderer texture sampler",
        });
        if (!sampler) {
            device->destroy_texture_view(resource.view);
            device->destroy_texture(resource.texture);
            resource.view = {};
            resource.texture = {};
            return unexpected(graphics_error_from_rhi(sampler.error(), "create RHI texture sampler"));
        }
        resource.sampler = *sampler;

        if (!resource.pixel_data.empty()) {
            const span<const std::byte> pixels{resource.pixel_data.data(), resource.pixel_data.size()};
            if (Core::RendererResult upload = upload_texture_rgba(
                    resource, resource.width, resource.height, resource.format, pixels);
                !upload.has_value()) {
                device->destroy_sampler(resource.sampler);
                device->destroy_texture_view(resource.view);
                device->destroy_texture(resource.texture);
                resource.sampler = {};
                resource.view = {};
                resource.texture = {};
                return upload;
            }
        }
        return {};
    }

    /// Clears placeholder texture.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param color `color` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::clear_placeholder_texture(TextureHandle handle, RHI::ClearColor color) {
        ZoneScopedN("Renderer::clear_placeholder_texture");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot clear a texture without an RHI device.");
        }
        TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->texture) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "clear_placeholder_texture: unknown or not-yet-created texture handle.");
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer texture placeholder clear"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create placeholder clear encoder"));
        }

        const RHI::TextureBarrier to_transfer{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::None,
            .src_access = RHI::AccessFlags::None,
            .dst_stage = RHI::PipelineStage::Transfer,
            .dst_access = RHI::AccessFlags::TransferWrite,
            .old_layout = RHI::TextureLayout::Undefined,
            .new_layout = RHI::TextureLayout::TransferDst,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_transfer, 1});
        (*encoder)->clear_color_texture(resource->texture, color, RHI::TextureSubresourceRange{});
        const RHI::TextureBarrier to_sampled{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::Transfer,
            .src_access = RHI::AccessFlags::TransferWrite,
            .dst_stage = RHI::PipelineStage::FragmentShader,
            .dst_access = RHI::AccessFlags::ShaderRead,
            .old_layout = RHI::TextureLayout::TransferDst,
            .new_layout = RHI::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish placeholder clear encoder"));
        }
        auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer texture placeholder clear fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create placeholder clear fence"));
        }
        const array command_buffers{*command_buffer};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer texture placeholder clear submit",
        };
        if (auto submitted = device->submit(submit_desc); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit placeholder clear"));
        }
        auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        if (!waited) {
            return unexpected(graphics_error_from_rhi(waited.error(), "wait placeholder clear fence"));
        }
        if (!*waited) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "wait placeholder clear fence: vkWaitForFences timed out.");
        }
        return {};
    }

    /// Submits texture upload.
    ///
    /// @param resource `resource` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param staging `staging` value used by the operation.
    /// @param staging_offset Offset from the beginning of the relevant range or buffer.
    /// @param queue Queue used or affected by the operation.
    /// @param d3d12_padded_rows `d3d12_padded_rows` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<TextureUploadSubmission> Renderer::submit_texture_upload(
        TextureResource &resource, u32 width, u32 height, RHI::Format format, RHI::BufferHandle staging,
        u64 staging_offset, RHI::QueueLane queue, bool d3d12_padded_rows) {
        ZoneScopedN("Renderer::submit_texture_upload");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Cannot upload texture data without an RHI device."});
        }
        const u64 copy_alignment = texture_copy_offset_alignment(format);
        if (resource.width != width || resource.height != height || resource.format != format ||
            resource.mip_levels == 0 || copy_alignment == 0 || staging_offset % copy_alignment != 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Texture upload description does not match the destination resource."});
        }


        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.queue = queue, .label = "renderer texture upload"});
        if (!encoder) {
            return unexpected(graphics_error_from_rhi(encoder.error(), "create texture upload encoder"));
        }

        const RHI::TextureBarrier to_transfer{
            .texture = resource.texture,
            .src_stage = RHI::PipelineStage::None,
            .src_access = RHI::AccessFlags::None,
            .dst_stage = RHI::PipelineStage::Transfer,
            .dst_access = RHI::AccessFlags::TransferWrite,
            .old_layout = RHI::TextureLayout::Undefined,
            .new_layout = RHI::TextureLayout::TransferDst,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_transfer, 1});

        u64 level_offset = staging_offset;
        u32 level_width = width;
        u32 level_height = height;
        for (u32 level = 0; level < resource.mip_levels; ++level) {
            const u64 tight_row_bytes = texture_data_bytes(format, level_width, 1);
            const u64 row_pitch = d3d12_padded_rows ? align_up(tight_row_bytes, 256) : tight_row_bytes;
            const u32 buffer_row_length = d3d12_padded_rows && tight_row_bytes != 0
                                              ? static_cast<u32>(static_cast<u64>(level_width) * row_pitch / tight_row_bytes)
                                              : 0;
            const RHI::BufferTextureCopy copy{
                .buffer_offset = level_offset,
                .buffer_row_length = buffer_row_length,
                .buffer_image_height = d3d12_padded_rows ? level_height : 0,
                .mip_level = level,
                .base_array_layer = 0,
                .array_layer_count = 1,
                .texture_offset = RHI::Offset3D{0, 0, 0},
                .texture_extent = RHI::Extent3D{.width = level_width, .height = level_height, .depth_or_layers = 1},
            };
            (*encoder)->copy_buffer_to_texture(staging, resource.texture, copy);
            level_offset += d3d12_padded_rows
                                ? row_pitch * level_height
                                : texture_data_bytes(format, level_width, level_height);
            if (level + 1u < resource.mip_levels) {
                level_offset = align_up(level_offset, d3d12_padded_rows ? 512 : copy_alignment);
            }
            level_width = std::max(level_width / 2u, 1u);
            level_height = std::max(level_height / 2u, 1u);
        }

        const RHI::TextureBarrier to_sampled{
            .texture = resource.texture,
            .src_stage = RHI::PipelineStage::Transfer,
            .src_access = RHI::AccessFlags::TransferWrite,
            .dst_stage = RHI::PipelineStage::FragmentShader,
            .dst_access = RHI::AccessFlags::ShaderRead,
            .old_layout = RHI::TextureLayout::TransferDst,
            .new_layout = RHI::TextureLayout::ShaderReadOnly,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_sampled, 1});

        auto command_buffer = (*encoder)->finish();
        if (!command_buffer) {
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish texture upload encoder"));
        }

        auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer texture upload fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(fence.error(), "create texture upload fence"));
        }

        const array command_buffers{*command_buffer};
        RHI::SubmitDesc submit_desc{
            .queue = queue,
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer texture upload submit",
        };
        if (auto submitted = device->submit(submit_desc); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit texture upload"));
        }
        return TextureUploadSubmission{*command_buffer, *fence};
    }

    /// Uploads texture rgba using the supplied arguments and current state.
    ///
    /// @param resource `resource` value used by the operation.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param data Data consumed or referenced by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::upload_texture_rgba(TextureResource &resource, u32 width, u32 height,
                                                       RHI::Format format, span<const std::byte> data) {
        ZoneScopedN("Renderer::upload_texture_rgba");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot upload texture data without an RHI device.");
        }

        const bool d3d12_padded_rows = device->backend_type() == RHI::BackendType::D3D12;
        vector<std::byte> padded_data;
        span<const std::byte> upload_data = data;
        if (d3d12_padded_rows) {
            u64 source_offset = 0;
            u64 destination_offset = 0;
            u32 level_width = width;
            u32 level_height = height;
            for (u32 level = 0; level < resource.mip_levels; ++level) {
                const u64 tight_row_bytes = texture_data_bytes(format, level_width, 1);
                const u64 row_pitch = align_up(tight_row_bytes, 256);
                destination_offset = align_up(destination_offset, 512);
                const u64 required = destination_offset + row_pitch * level_height;
                padded_data.resize(static_cast<usize>(required));
                for (u32 row = 0; row < level_height; ++row) {
                    std::memcpy(padded_data.data() + destination_offset + static_cast<u64>(row) * row_pitch,
                                data.data() + source_offset + static_cast<u64>(row) * tight_row_bytes,
                                static_cast<usize>(tight_row_bytes));
                }
                source_offset += texture_data_bytes(format, level_width, level_height);
                destination_offset = required;
                level_width = std::max(level_width / 2u, 1u);
                level_height = std::max(level_height / 2u, 1u);
            }
            upload_data = padded_data;
        }

        auto staging = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(upload_data.size()),
            .usage = RHI::BufferUsage::TransferSrc,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "renderer texture staging",
        });
        if (!staging) {
            return unexpected(graphics_error_from_rhi(staging.error(), "create texture staging buffer"));
        }
        if (auto written = device->write_buffer(*staging, 0, upload_data); !written) {
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(written.error(), "write texture staging buffer"));
        }

        auto submitted = submit_texture_upload(resource, width, height, format, *staging, 0, {}, d3d12_padded_rows);
        if (!submitted) {
            device->destroy_buffer(*staging);
            return unexpected(submitted.error());
        }

        auto waited = device->wait_fences(span<const RHI::FenceHandle>{&submitted->fence, 1}, true);
        if (!waited) {
            device->destroy_fence(submitted->fence);
            device->destroy_command_buffer(submitted->command_buffer);
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(waited.error(), "wait texture upload fence"));
        }
        if (!*waited) {


            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "wait texture upload fence: vkWaitForFences timed out.");
        }

        device->destroy_fence(submitted->fence);
        device->destroy_command_buffer(submitted->command_buffer);
        device->destroy_buffer(*staging);
        return {};
    }

    /// Finds or creates the default white texture required by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<TextureHandle> Renderer::ensure_default_white_texture() {
        ZoneScopedN("Renderer::ensure_default_white_texture");
        if (TextureResource *existing = texture(default_white_texture_)) {
            return existing->handle;
        }
        const array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        auto handle = create_texture(1, 1, RHI::Format::RGBA8Unorm,
                                     span<const std::byte>{white.data(), white.size()}, "renderer default white");
        if (!handle) {
            return handle;
        }
        default_white_texture_ = *handle;
        return *handle;
    }

    /// Performs the adopt texture operation for `Renderer` using the supplied arguments.
    ///
    /// @param texture Texture used or affected by the operation.
    /// @param view `view` value used by the operation.
    /// @param sampler Sampler used or affected by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<TextureHandle> Renderer::adopt_texture(RHI::TextureHandle texture, RHI::TextureViewHandle view,
                                                                   RHI::SamplerHandle sampler, const char *label) {
        ZoneScopedN("Renderer::adopt_texture");
        if (!texture || !view) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                          "Cannot adopt a texture without a valid texture and view."});
        }

        TextureResource resource{};
        resource.handle = TextureHandle{static_cast<u64>(textures_.size() + 1)};
        resource.label = label ? label : "";
        resource.texture = texture;
        resource.view = view;
        resource.sampler = sampler;
        resource.owns_gpu_resources = false;
        resource.alive = true;
        textures_.push_back(resource);
        return textures_.back().handle;
    }

    /// Destroys the texture identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_texture(TextureHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_texture");
        TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->externally_destroyable) {
            return;
        }
        if (resource->owns_gpu_resources) {
            if (RHI::RhiDevice *device = rhi_device()) {
                if (resource->sampler) {
                    device->destroy_sampler(resource->sampler);
                }
                if (resource->view) {
                    device->destroy_texture_view(resource->view);
                }
                if (resource->texture) {
                    device->destroy_texture(resource->texture);
                }
            }
        }
        *resource = {};
    }

    /// Performs the texture operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    TextureResource *Renderer::texture(TextureHandle handle) noexcept {
        ZoneScopedN("Renderer::texture");
        if (!handle || handle.value > textures_.size()) {
            return nullptr;
        }
        TextureResource &resource = textures_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    /// Performs the texture operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const TextureResource *Renderer::texture(TextureHandle handle) const noexcept {
        ZoneScopedN("Renderer::texture");
        if (!handle || handle.value > textures_.size()) {
            return nullptr;
        }
        const TextureResource &resource = textures_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

} // namespace SFT::Renderer
