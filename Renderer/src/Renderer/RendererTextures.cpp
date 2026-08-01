#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    namespace {

        // Total pixel-data byte size for the (single-plane, single-mip) formats the renderer texture
        // path accepts, given the format's own packing rules — per-texel for uncompressed formats,
        // per-4x4-block for block-compressed ones (format_is_block_compressed). 0 for anything else —
        // the caller rejects an unsupported format rather than guessing.
        [[nodiscard]] u64 texture_data_bytes(RHI::Format format, u32 width, u32 height) noexcept {
            if (RHI::format_is_block_compressed(format)) {
                u32 bytes_per_block = 0;
                switch (format) {
                    case RHI::Format::BC7Unorm:
                    case RHI::Format::BC7UnormSrgb:
                    case RHI::Format::BC5Unorm: bytes_per_block = 16; break;
                    case RHI::Format::BC4Unorm: bytes_per_block = 8; break;
                    default: return 0;
                }
                const u64 blocks_wide = (static_cast<u64>(width) + 3) / 4;
                const u64 blocks_high = (static_cast<u64>(height) + 3) / 4;
                return blocks_wide * blocks_high * bytes_per_block;
            }

            u32 texel_size = 0;
            switch (format) {
                case RHI::Format::R8Unorm: texel_size = 1; break;
                case RHI::Format::RGBA8Unorm:
                case RHI::Format::RGBA8UnormSrgb: texel_size = 4; break;
                default: return 0;
            }
            return static_cast<u64>(width) * height * texel_size;
        }

    } // namespace

    Core::RendererExpected<TextureHandle> Renderer::create_texture(u32 width, u32 height, RHI::Format format,
                                                                   span<const std::byte> data, const char *label) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a texture without an RHI device."});
        }
        if (width == 0 || height == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a texture with a zero dimension."});
        }
        const u64 expected_bytes = texture_data_bytes(format, width, height);
        if (expected_bytes == 0) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Unsupported texture format for renderer create_texture."});
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
        resource.format = format;
        resource.pixel_data.assign(data.begin(), data.end());
        resource.alive = true;

        if (Core::RendererResult created = create_owned_texture_gpu(resource); !created.has_value()) {
            return unexpected(created.error());
        }
        textures_.push_back(std::move(resource));
        return textures_.back().handle;
    }

    Core::RendererResult Renderer::create_owned_texture_gpu(TextureResource &resource) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot create an owned texture without an RHI device.");
        }
        if (!resource.owns_gpu_resources || resource.width == 0 || resource.height == 0 ||
            resource.format == RHI::Format::Undefined) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Owned texture is missing a valid CPU replay description.");
        }

        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = resource.format,
            .extent = RHI::Extent3D{.width = resource.width, .height = resource.height, .depth_or_layers = 1},
            .mip_levels = 1,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst,
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

        auto sampler = device->create_sampler(RHI::SamplerDesc{.label = "renderer texture sampler"});
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

    Core::RendererResult Renderer::upload_texture_rgba(TextureResource &resource, u32 width, u32 height,
                                                       RHI::Format format, span<const std::byte> data) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot upload texture data without an RHI device.");
        }

        // Stage the pixels in a host-visible buffer, then copy into the device-local texture through a
        // one-shot command buffer (Undefined→TransferDst→ShaderReadOnly). Same shape as the mesh staging
        // path; the frame graph will later route bulk uploads onto a dedicated transfer queue instead.
        auto staging = device->create_buffer(RHI::BufferDesc{
            .size = static_cast<u64>(data.size()),
            .usage = RHI::BufferUsage::TransferSrc,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "renderer texture staging",
        });
        if (!staging) {
            return unexpected(graphics_error_from_rhi(staging.error(), "create texture staging buffer"));
        }
        if (auto written = device->write_buffer(*staging, 0, data); !written) {
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(written.error(), "write texture staging buffer"));
        }

        auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "renderer texture upload"});
        if (!encoder) {
            device->destroy_buffer(*staging);
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

        const RHI::BufferTextureCopy copy{
            .buffer_offset = 0,
            .mip_level = 0,
            .base_array_layer = 0,
            .array_layer_count = 1,
            .texture_offset = RHI::Offset3D{0, 0, 0},
            .texture_extent = RHI::Extent3D{.width = width, .height = height, .depth_or_layers = 1},
        };
        (void)format;
        (*encoder)->copy_buffer_to_texture(*staging, resource.texture, copy);

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
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish texture upload encoder"));
        }

        auto fence = device->create_fence(RHI::FenceDesc{.label = "renderer texture upload fence"});
        if (!fence) {
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(fence.error(), "create texture upload fence"));
        }

        const array command_buffers{*command_buffer};
        RHI::SubmitDesc submit_desc{
            .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
            .fence = *fence,
            .flags = RHI::SubmitFlags::OneShot,
            .label = "renderer texture upload submit",
        };
        if (auto submitted = device->submit(submit_desc); !submitted) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(submitted.error(), "submit texture upload"));
        }
        if (auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true); !waited) {
            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
            device->destroy_buffer(*staging);
            return unexpected(graphics_error_from_rhi(waited.error(), "wait texture upload fence"));
        }

        device->destroy_fence(*fence);
        device->destroy_command_buffer(*command_buffer);
        device->destroy_buffer(*staging);
        return {};
    }

    Core::RendererExpected<TextureHandle> Renderer::ensure_default_white_texture() {
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

    Core::RendererExpected<TextureHandle> Renderer::adopt_texture(RHI::TextureHandle texture, RHI::TextureViewHandle view,
                                                                   RHI::SamplerHandle sampler, const char *label) {
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

    void Renderer::destroy_texture(TextureHandle handle) noexcept {
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

    TextureResource *Renderer::texture(TextureHandle handle) noexcept {
        if (!handle || handle.value > textures_.size()) {
            return nullptr;
        }
        TextureResource &resource = textures_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const TextureResource *Renderer::texture(TextureHandle handle) const noexcept {
        if (!handle || handle.value > textures_.size()) {
            return nullptr;
        }
        const TextureResource &resource = textures_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

} // namespace SFT::Renderer
