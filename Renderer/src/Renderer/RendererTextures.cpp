#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <algorithm>
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
#include <Renderer/TextureUploadPacking.hpp>
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
        /// Returns how many rows of packed data one mip level occupies.
        ///
        /// A block-compressed level stores one row per *block* row, covering four pixel rows, so
        /// walking pixel rows would read four times the data that is actually there. Uncompressed
        /// formats store one row per pixel row, where the two agree.
        ///
        /// @param format Texture format.
        /// @param height Pixel height of the level.
        ///
        /// @return The number of stored rows.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 texture_row_count(RHI::Format format, u32 height) noexcept {
            if (height == 0) {
                return 0;
            }
            return RHI::format_is_block_compressed(format) ? (height + 3u) / 4u : height;
        }

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
                // The format an HDR source texture (an EXR environment map, a PQ AVIF) is uploaded
                // in: no 8-bit format can hold scene-linear light above display white, and no BC
                // format this engine can encode holds float at all.
                case RHI::Format::RGBA16Float: texel_size = 8; break;
                // Displacement heightfields / hierarchies (Renderer::create_displaced_material): no R16Unorm
                // exists in the RHI, so height data is stored as exact 32-bit float.
                case RHI::Format::R32Float: texel_size = 4; break;
                case RHI::Format::RG32Float: texel_size = 8; break;
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
                case RHI::Format::RGBA16Float: return 8;
                case RHI::Format::R32Float: return 4;
                case RHI::Format::RG32Float: return 8;
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


    } // namespace

    /// Returns the number of bytes a complete mip chain occupies in `format`.
    ///
    /// @param format Format used for the resource, render target, or conversion.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param mip_levels `mip_levels` value used by the operation.
    ///
    /// @return Returns the requested count or size; 0 when the format is unsupported.
    /// @note This function does not throw exceptions.
    u64 texture_mip_chain_byte_size(RHI::Format format, u32 width, u32 height, u32 mip_levels) noexcept {
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
                                                                   u32 mip_levels, bool nearest_sampling) {
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
        const u64 expected_bytes = texture_mip_chain_byte_size(format, width, height, mip_levels);
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
        resource.nearest_sampling = nearest_sampling;
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

        ++resource.revision;
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

        const RHI::Filter filter = resource.nearest_sampling ? RHI::Filter::Nearest : RHI::Filter::Linear;
        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = filter,
            .mag_filter = filter,
            .mipmap_mode = RHI::MipmapMode::Linear,
            .max_lod = static_cast<f32>(resource.mip_levels - 1u),
            .max_anisotropy = resource.nearest_sampling ? 0.0f : 8.0f,
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
    /// @param padded_rows `padded_rows` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererExpected<TextureUploadSubmission> Renderer::submit_texture_upload(
        TextureResource &resource, u32 width, u32 height, RHI::Format format, RHI::BufferHandle staging,
        u64 staging_offset, RHI::QueueLane queue, bool padded_rows) {
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
            const u64 row_pitch = padded_rows ? align_up(tight_row_bytes, 256) : tight_row_bytes;
            // Expressed so the backend recovers exactly `row_pitch` from it: it computes
            // ceil(row_length / block) * element_bytes, so row_length must be whole blocks.
            //
            // Scaling the width by row_pitch/tight_row_bytes looks equivalent and is not: once a
            // level is four pixels wide or narrower, blocks-per-row is clamped to one by the ceil,
            // the ratio stops tracking the width, and the smallest mips of a compressed texture
            // produce an unaligned pitch that D3D12 rejects.
            const u32 block_extent = RHI::format_is_block_compressed(format) ? 4u : 1u;
            const u64 element_bytes = texture_data_bytes(format, block_extent, block_extent);
            const u32 buffer_row_length =
                padded_rows && element_bytes != 0 && row_pitch % element_bytes == 0
                    ? static_cast<u32>((row_pitch / element_bytes) * block_extent)
                    : 0;
            const RHI::BufferTextureCopy copy{
                .buffer_offset = level_offset,
                .buffer_row_length = buffer_row_length,
                .buffer_image_height = padded_rows ? level_height : 0,
                .mip_level = level,
                .base_array_layer = 0,
                .array_layer_count = 1,
                .texture_offset = RHI::Offset3D{0, 0, 0},
                .texture_extent = RHI::Extent3D{.width = level_width, .height = level_height, .depth_or_layers = 1},
            };
            (*encoder)->copy_buffer_to_texture(staging, resource.texture, copy);
            // Must match upload_texture_rgba's packing stride exactly, including the block-row
            // count — the two walk the same buffer from opposite ends.
            level_offset += padded_rows
                                ? row_pitch * texture_row_count(format, level_height)
                                : texture_data_bytes(format, level_width, level_height);
            if (level + 1u < resource.mip_levels) {
                level_offset = align_up(level_offset, padded_rows ? 512 : copy_alignment);
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

        // D3D12 and WebGPU both require each row of a buffer-to-texture copy to start on a 256-byte
        // boundary; Vulkan does not, and packing tightly there saves the repack. Shared with
        // TextureStreamer, which stages its own upload buffer and needs the identical transform --
        // see TextureUploadPacking.hpp for why the two must stay in exact agreement.
        const RHI::BackendType backend = device->backend_type();
        const bool padded_rows = backend_requires_padded_texture_rows(backend);
        vector<std::byte> padded_data;
        span<const std::byte> upload_data = data;
        if (padded_rows) {
            padded_data = pack_texture_upload_rows(format, width, height, resource.mip_levels, data);
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

        auto submitted = submit_texture_upload(resource, width, height, format, *staging, 0, {}, padded_rows);
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

    /// Finds or creates the default flat-normal texture used for unbound normal-map slots.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<TextureHandle> Renderer::ensure_default_flat_normal_texture() {
        ZoneScopedN("Renderer::ensure_default_flat_normal_texture");
        if (TextureResource *existing = texture(default_flat_normal_texture_)) {
            return existing->handle;
        }
        // (0.5, 0.5, 1) is the tangent-space encoding of "no perturbation": it unpacks to +Z, which
        // is the interpolated vertex normal itself.
        const array<std::byte, 4> flat{std::byte{0x80}, std::byte{0x80}, std::byte{0xFF}, std::byte{0xFF}};
        auto handle = create_texture(1, 1, RHI::Format::RGBA8Unorm,
                                     span<const std::byte>{flat.data(), flat.size()},
                                     "renderer default flat normal");
        if (!handle) {
            return handle;
        }
        default_flat_normal_texture_ = *handle;
        return *handle;
    }

    Core::RendererExpected<TextureHandle> Renderer::ensure_default_transparent_black_texture() {
        ZoneScopedN("Renderer::ensure_default_transparent_black_texture");
        if (TextureResource *existing = texture(default_transparent_black_texture_)) {
            return existing->handle;
        }
        const array<std::byte, 4> clear{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
        auto handle = create_texture(1, 1, RHI::Format::RGBA8Unorm, span<const std::byte>{clear.data(), clear.size()},
                                     "renderer default transparent black");
        if (!handle) {
            return handle;
        }
        default_transparent_black_texture_ = *handle;
        return *handle;
    }

    Core::RendererExpected<RenderGraphTextureHandle> Renderer::import_default_texture(RenderGraph &graph, TextureHandle handle,
                                                                                     const char *label) {
        const TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->texture || !resource->view) {
            return unexpected(Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                           "A default texture is missing its RHI resources."));
        }
        return graph.import_texture(RenderGraphImportedTextureDesc{
            .texture = resource->texture,
            .default_view = resource->view,
            .format = RHI::Format::RGBA8Unorm,
            .extent = RHI::Extent3D{.width = resource->width, .height = resource->height, .depth_or_layers = 1},
            .usage = RHI::TextureUsage::Sampled,
            .label = label,
        });
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
    Core::RendererResult Renderer::update_texture(TextureHandle handle, span<const std::byte> data) {
        ZoneScopedN("Renderer::update_texture");
        TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->alive) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot update an unknown texture.");
        }
        if (!resource->owns_gpu_resources) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot update an adopted texture; its owner writes it.");
        }
        if (data.size() != resource->pixel_data.size()) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Texture update size mismatch: the new pixels must match the texture's size, format and mip chain.");
        }
        resource->pixel_data.assign(data.begin(), data.end());
        if (!resource->texture) {
            return {}; // no GPU copy right now (mid device switch); the replay picks up the new pixels
        }
        return upload_texture_rgba(*resource, resource->width, resource->height, resource->format,
                                   span<const std::byte>{resource->pixel_data.data(), resource->pixel_data.size()});
    }

    void Renderer::destroy_texture(TextureHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_texture");
        TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->externally_destroyable) {
            return;
        }
        if (resource->owns_gpu_resources) {
            // Not destroyed on the spot: a frame that is still in flight may be sampling it (and a UI bind group built
            // over its view may still be queued). The frame slot that is submitted next frees it once its fence signals.
            retire_texture_gpu(resource->texture, resource->view, resource->sampler);
        }
        // The slot stays (handles are indices and are never reused), but a later `revision` can never match this one.
        const u64 revision = resource->revision + 1;
        *resource = {};
        resource->revision = revision;
    }

    void Renderer::note_texture_demand(span<const std::pair<TextureHandle, f32>> drawn) {
        if (drawn.empty()) {
            return;
        }
        auto demand = texture_demand_.lock();
        for (const auto &[handle, pixels] : drawn) {
            f32 &largest = (*demand)[handle.value];
            largest = std::max(largest, pixels);
        }
    }

    vector<std::pair<TextureHandle, f32>> Renderer::take_texture_demand() {
        vector<std::pair<TextureHandle, f32>> out;
        auto demand = texture_demand_.lock();
        out.reserve(demand->size());
        for (const auto &[value, pixels] : *demand) {
            out.emplace_back(TextureHandle{value}, pixels);
        }
        demand->clear();
        return out;
    }

    void Renderer::retire_texture_gpu(RHI::TextureHandle texture, RHI::TextureViewHandle view,
                                      RHI::SamplerHandle sampler) noexcept {
        if (!texture && !view && !sampler) {
            return;
        }
        auto retired = retired_texture_gpu_.lock();
        if (texture) {
            retired->textures.push_back(texture);
        }
        if (view) {
            retired->views.push_back(view);
        }
        if (sampler) {
            retired->samplers.push_back(sampler);
        }
    }

    void Renderer::move_retired_texture_gpu_to_slot(FrameInFlight &slot) {
        auto retired = retired_texture_gpu_.lock();
        slot.transient_textures.insert(slot.transient_textures.end(), retired->textures.begin(), retired->textures.end());
        slot.transient_texture_views.insert(slot.transient_texture_views.end(), retired->views.begin(),
                                            retired->views.end());
        slot.transient_samplers.insert(slot.transient_samplers.end(), retired->samplers.begin(), retired->samplers.end());
        retired->textures.clear();
        retired->views.clear();
        retired->samplers.clear();
    }

    void Renderer::destroy_retired_texture_gpu_now() noexcept {
        RetiredTextureGpu retired;
        {
            auto guard = retired_texture_gpu_.lock();
            retired = std::move(*guard);
            *guard = {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        for (RHI::SamplerHandle sampler : retired.samplers) {
            device->destroy_sampler(sampler);
        }
        for (RHI::TextureViewHandle view : retired.views) {
            device->destroy_texture_view(view);
        }
        for (RHI::TextureHandle texture : retired.textures) {
            device->destroy_texture(texture);
        }
    }

    Core::RendererResult Renderer::replace_texture(TextureHandle handle, u32 width, u32 height, RHI::Format format,
                                                   span<const std::byte> data, u32 mip_levels) {
        ZoneScopedN("Renderer::replace_texture");
        TextureResource *resource = texture(handle);
        if (resource == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot replace an unknown texture.");
        }
        if (!resource->owns_gpu_resources) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot replace an adopted texture; its owner writes it.");
        }
        if (width == 0 || height == 0) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot replace a texture with a zero dimension.");
        }
        const u64 expected_bytes = texture_mip_chain_byte_size(format, width, height, mip_levels);
        if (expected_bytes == 0 || static_cast<u64>(data.size()) != expected_bytes) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Texture replacement data does not match the format, size and mip count it was given.");
        }

        if (!resource->texture) {
            // Mid device switch: there is no GPU copy to swap. The replay creates it from the new description.
            resource->width = width;
            resource->height = height;
            resource->mip_levels = mip_levels;
            resource->format = format;
            resource->pixel_data.assign(data.begin(), data.end());
            ++resource->revision;
            return {};
        }

        TextureResource fresh{};
        fresh.handle = handle;
        fresh.label = resource->label;
        fresh.width = width;
        fresh.height = height;
        fresh.mip_levels = mip_levels;
        fresh.format = format;
        fresh.revision = resource->revision;
        fresh.pixel_data.assign(data.begin(), data.end());
        fresh.concurrent_queue_classes = resource->concurrent_queue_classes;
        fresh.nearest_sampling = resource->nearest_sampling;
        fresh.alive = true;
        if (Core::RendererResult created = create_owned_texture_gpu(fresh); !created.has_value()) {
            return created;
        }

        // `texture()` pointers stay valid: creating GPU objects does not touch `textures_`.
        retire_texture_gpu(resource->texture, resource->view, resource->sampler);
        resource->texture = fresh.texture;
        resource->view = fresh.view;
        resource->sampler = fresh.sampler;
        resource->width = width;
        resource->height = height;
        resource->mip_levels = mip_levels;
        resource->format = format;
        resource->revision = fresh.revision;
        resource->pixel_data = std::move(fresh.pixel_data);

        // Material bind groups bake in the view they were built with; the old one is on its way out.
        for (MaterialInstanceResource &instance : material_instances_) {
            if (!instance.alive) {
                continue;
            }
            const bool uses_texture = std::ranges::any_of(
                instance.textures, [handle](const MaterialTextureBinding &binding) { return binding.texture == handle; });
            if (!uses_texture) {
                continue;
            }
            ++instance.content_revision;
            for (MaterialInstanceFrame &frame : instance.frames) {
                frame.bind_groups_dirty = true;
            }
        }
        return {};
    }

    Core::RendererResult Renderer::update_texture_regions(TextureHandle handle, span<const TextureRegion> regions,
                                                          span<const std::byte> pixels) {
        ZoneScopedN("Renderer::update_texture_regions");
        TextureResource *resource = texture(handle);
        if (resource == nullptr || !resource->owns_gpu_resources) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot update regions of an unknown or adopted texture.");
        }
        const bool eight_bit = resource->format == RHI::Format::RGBA8Unorm || resource->format == RHI::Format::RGBA8UnormSrgb;
        if (!eight_bit || resource->mip_levels != 1 ||
            resource->pixel_data.size() != static_cast<usize>(resource->width) * resource->height * 4u) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Region updates need an uncompressed 8-bit RGBA texture with a single mip level.");
        }
        u64 total_region_bytes = 0;
        for (const TextureRegion &region : regions) {
            if (region.width == 0 || region.height == 0 || region.x > resource->width ||
                region.width > resource->width - region.x || region.y > resource->height ||
                region.height > resource->height - region.y) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "A texture region lies outside the texture.");
            }
            total_region_bytes += static_cast<u64>(region.width) * region.height * 4u;
        }
        if (total_region_bytes != static_cast<u64>(pixels.size())) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Region pixel data does not match the regions' total size.");
        }
        if (regions.empty()) {
            return {};
        }

        // The replay copy first, so a device switch restores what is on screen.
        {
            usize source_offset = 0;
            for (const TextureRegion &region : regions) {
                const usize row_bytes = static_cast<usize>(region.width) * 4u;
                for (u32 row = 0; row < region.height; ++row) {
                    const usize destination =
                        (static_cast<usize>(region.y + row) * resource->width + region.x) * 4u;
                    std::memcpy(resource->pixel_data.data() + destination, pixels.data() + source_offset, row_bytes);
                    source_offset += row_bytes;
                }
            }
        }
        if (!resource->texture) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot update a texture without an RHI device.");
        }
        const bool padded_rows = backend_requires_padded_texture_rows(device->backend_type());
        // Every region starts on a 512-byte boundary in the staging buffer: that satisfies the strictest backend.
        constexpr u64 kRegionAlignment = 512;
        vector<std::byte> staging_bytes;
        vector<RHI::BufferTextureCopy> copies;
        copies.reserve(regions.size());
        usize source_offset = 0;
        for (const TextureRegion &region : regions) {
            const u64 tight_row = static_cast<u64>(region.width) * 4u;
            const u64 pitch = padded_rows ? align_up(tight_row, 256) : tight_row;
            const u64 offset = align_up(static_cast<u64>(staging_bytes.size()), kRegionAlignment);
            staging_bytes.resize(static_cast<usize>(offset + pitch * region.height), std::byte{0});
            for (u32 row = 0; row < region.height; ++row) {
                std::memcpy(staging_bytes.data() + offset + pitch * row, pixels.data() + source_offset,
                            static_cast<usize>(tight_row));
                source_offset += static_cast<usize>(tight_row);
            }
            copies.push_back(RHI::BufferTextureCopy{
                .buffer_offset = offset,
                .buffer_row_length = padded_rows ? static_cast<u32>(pitch / 4u) : 0u,
                .buffer_image_height = 0,
                .mip_level = 0,
                .base_array_layer = 0,
                .array_layer_count = 1,
                .texture_offset = RHI::Offset3D{static_cast<i32>(region.x), static_cast<i32>(region.y), 0},
                .texture_extent = RHI::Extent3D{.width = region.width, .height = region.height, .depth_or_layers = 1},
            });
        }

        auto staging = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(staging_bytes.size()),
            .usage = RHI::BufferUsage::TransferSrc,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "renderer texture region staging",
        });
        if (!staging) {
            return unexpected(graphics_error_from_rhi(staging.error(), "create texture region staging buffer"));
        }
        const auto release_staging = [&] { device->destroy_buffer(*staging); };
        if (auto written = device->write_buffer(*staging, 0, span<const std::byte>{staging_bytes});
            !written) {
            release_staging();
            return unexpected(graphics_error_from_rhi(written.error(), "write texture region staging buffer"));
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer texture region update"});
        if (!encoder) {
            release_staging();
            return unexpected(graphics_error_from_rhi(encoder.error(), "create texture region encoder"));
        }
        // The texture keeps its contents (it is only partly overwritten) and is read by the UI, so it goes
        // shader-read -> transfer-destination -> shader-read rather than starting from `Undefined`.
        const RHI::TextureBarrier to_transfer{
            .texture = resource->texture,
            .src_stage = RHI::PipelineStage::FragmentShader,
            .src_access = RHI::AccessFlags::ShaderRead,
            .dst_stage = RHI::PipelineStage::Transfer,
            .dst_access = RHI::AccessFlags::TransferWrite,
            .old_layout = RHI::TextureLayout::ShaderReadOnly,
            .new_layout = RHI::TextureLayout::TransferDst,
        };
        (*encoder)->barrier({}, {}, span<const RHI::TextureBarrier>{&to_transfer, 1});
        for (const RHI::BufferTextureCopy &copy : copies) {
            (*encoder)->copy_buffer_to_texture(*staging, resource->texture, copy);
        }
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
            release_staging();
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish texture region encoder"));
        }
        auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer texture region fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            release_staging();
            return unexpected(graphics_error_from_rhi(fence.error(), "create texture region fence"));
        }
        const array command_buffers{*command_buffer};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer texture region submit",
        };
        if (auto submitted = device->submit(submit_desc); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            release_staging();
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit texture region update"));
        }
        auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true);
        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        release_staging();
        if (!waited) {
            return unexpected(graphics_error_from_rhi(waited.error(), "wait texture region fence"));
        }
        if (!*waited) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "wait texture region fence: timed out.");
        }
        return {};
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
