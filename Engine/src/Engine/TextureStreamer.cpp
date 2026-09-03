#include <Engine/TextureStreamer.hpp>
#include <Engine/ImageDecode.hpp>
#include <Engine/TextureCompression.hpp>
#include <Engine/TextureMipChain.hpp>

#include <Async/Affinity.hpp>
#include <Async/Mutex.hpp>
#include <Renderer/Renderer.hpp>


#include <Core/StreamingIo.hpp>

#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::vector;

namespace SFT::Engine {

    namespace {


        /// Reads binary file streamed from the associated source.
        ///
        /// @param source Source value or resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `AssetErrorCode::IoFailure`, `AssetErrorCode::NotFound`.
        [[nodiscard]] AssetExpected<vector<std::byte>> read_binary_file_streamed(const std::filesystem::path &source) {


            if (auto bytes = Core::read_file_accelerated(source)) {
                return std::move(*bytes);
            }
            std::ifstream file(source, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                const AssetErrorCode code =
                    std::filesystem::exists(source) ? AssetErrorCode::IoFailure : AssetErrorCode::NotFound;
                return std::unexpected(AssetError{code, UString{"Could not open asset file '" + source.string() + "'."}, source});
            }
            const std::streamoff end = file.tellg();
            if (end < 0 || static_cast<u64>(end) > std::numeric_limits<usize>::max()) {
                return std::unexpected(AssetError{AssetErrorCode::IoFailure,
                                                   UString{"Could not determine the size of asset file '" + source.string() + "'."}, source});
            }
            vector<std::byte> bytes(static_cast<usize>(end));
            file.seekg(0, std::ios::beg);
            if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
                return std::unexpected(AssetError{AssetErrorCode::IoFailure,
                                                   UString{"Could not read the complete asset file '" + source.string() + "'."}, source});
            }
            return bytes;
        }

    } // namespace

    struct TextureStreamer::Impl {


        struct Entry {
            StreamingState state = StreamingState::Pending;
            SFT::Renderer::TextureHandle texture{};
            u32 width = 0;
            u32 height = 0;
            u64 byte_count = 0;
            vector<std::function<void(SFT::Renderer::TextureHandle)>> callbacks;
            std::optional<RHI::FenceHandle> upload_fence;
            RHI::CommandBufferHandle upload_command_buffer{};


            RHI::BufferHandle owned_staging_buffer{};
        };


        struct StagingChunk {
            RHI::BufferHandle buffer{};
            u64 last_user_id = 0;
        };

        static constexpr usize kRingSize = 4;
        static constexpr u64 kChunkBytes = 16ull * 1024 * 1024;

        /// Performs the impl operation for `TextureStreamer` using the supplied arguments.
        ///
        /// @param renderer_ref Renderer used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit Impl(SFT::Renderer::Renderer &renderer_ref) : renderer(renderer_ref), thread("TextureStreamer") {}


        /// Acquires chunk.
        ///
        /// @param bytes Size of the relevant data in bytes.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`, `RhiErrorCode::InvalidArgument`.
        [[nodiscard]] RHI::RhiExpected<StagingChunk *> acquire_chunk(u64 bytes) {
            ZoneScopedN("TextureStreamer::acquire_chunk");
            RHI::RhiDevice *device = renderer.rhi_device();
            if (device == nullptr) {
                return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "acquire_chunk: no RHI device."));
            }
            if (bytes > kChunkBytes) {
                return unexpected(RHI::rhi_error(RHI::RhiErrorCode::InvalidArgument, "acquire_chunk: request exceeds ring chunk size."));
            }
            if (!ring_initialized) {
                for (StagingChunk &chunk : ring) {
                    auto buffer = device->create_buffer(RHI::BufferDesc{
                        .size = kChunkBytes,
                        .usage = RHI::BufferUsage::TransferSrc,
                        .memory = RHI::MemoryLocation::HostUpload,
                        .label = "texture streamer staging ring chunk",
                    });
                    if (!buffer) {
                        return unexpected(buffer.error());
                    }
                    chunk.buffer = *buffer;
                }
                ring_initialized = true;
            }

            StagingChunk &chunk = ring[ring_cursor];
            ring_cursor = (ring_cursor + 1) % kRingSize;

            if (chunk.last_user_id != 0) {
                std::optional<RHI::FenceHandle> fence_to_wait;
                {
                    auto guard = entries.lock();
                    if (auto it = guard->find(chunk.last_user_id); it != guard->end() && it->second.state == StreamingState::Uploading) {
                        fence_to_wait = it->second.upload_fence;
                    }
                }


                if (fence_to_wait) {
                    auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence_to_wait, 1}, true);
                    if (!waited) {
                        return unexpected(waited.error());
                    }
                }
            }
            return &chunk;
        }

        SFT::Renderer::Renderer &renderer;
        Async::DedicatedThread thread;
        Async::Mutex<std::unordered_map<u64, Entry>> entries;
        std::atomic<u64> next_id{1};

        Async::Mutex<vector<u64>> submitted;


        std::atomic<i64> queue_depth{0};
        std::atomic<i64> in_flight_bytes{0};


        array<StagingChunk, kRingSize> ring{};
        usize ring_cursor = 0;
        bool ring_initialized = false;
    };

    /// Performs the texture streamer operation for `Engine` using the supplied arguments.
    ///
    /// @param renderer Renderer used or affected by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    TextureStreamer::TextureStreamer(SFT::Renderer::Renderer &renderer) : impl_(std::make_unique<Impl>(renderer)) {}

    /// Destroys the `Engine` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    TextureStreamer::~TextureStreamer() = default;

    /// Requests texture load using the supplied arguments and current state.
    ///
    /// @param source Source value or resource.
    /// @param color_space `color_space` value used by the operation.
    /// @param kind `kind` value used by the operation.
    /// @param placeholder `placeholder` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    StreamedTextureHandle TextureStreamer::request_texture_load(
        std::filesystem::path source, TextureColorSpace color_space, TextureKind kind,
        RHI::ClearColor placeholder, UString label) {
        ZoneScopedN("TextureStreamer::request_texture_load");
        auto encoded = read_binary_file_streamed(source);
        if (!encoded) {
            Foundation::log_error("TextureStreamer: failed to read '{}': {}", source.string(), encoded.error().message.cpp_string());
            return {};
        }
        auto decoded = Detail::decode_image_rgba8(*encoded, source);
        if (!decoded) {
            Foundation::log_error("TextureStreamer: failed to decode '{}': {}", source.string(), decoded.error().message.cpp_string());
            return {};
        }

        const bool srgb = color_space == TextureColorSpace::Srgb;
        RHI::Format format = srgb ? RHI::Format::RGBA8UnormSrgb : RHI::Format::RGBA8Unorm;
        auto generated_mips = Detail::generate_rgba8_mip_chain(
            decoded->pixels(), decoded->width, decoded->height, srgb);
        if (!generated_mips) {
            Foundation::log_error("TextureStreamer: failed to generate mip levels for '{}'.", source.string());
            return {};
        }
        Detail::TextureMipChain mip_chain = std::move(*generated_mips);
        vector<std::byte>{}.swap(decoded->pixels());
        const u32 mip_levels = mip_chain.mip_levels;
        span<const std::byte> upload_bytes{mip_chain.data.data(), mip_chain.data.size()};


        vector<std::byte> compressed_storage;
        RHI::RhiDevice *device = impl_->renderer.rhi_device();
        if (decoded->width >= 4 && decoded->height >= 4 && device != nullptr &&
            device->limits().supports_bc_texture_compression) {
            std::optional<vector<std::byte>> compressed;
            switch (kind) {
                case TextureKind::ColorOpaque:
                    compressed = Detail::compress_bc1_mip_chain(upload_bytes, decoded->width, decoded->height,
                        mip_levels, srgb);
                    break;
                case TextureKind::Mask:
                    compressed = Detail::compress_bc4_mip_chain(upload_bytes, decoded->width, decoded->height,
                        mip_levels);
                    break;
                case TextureKind::NormalMap:
                case TextureKind::MetallicRoughness:


                    compressed = Detail::compress_bc5_mip_chain(upload_bytes, decoded->width, decoded->height,
                        mip_levels);
                    break;
                case TextureKind::ColorAlpha:
                default:
                    compressed = Detail::compress_bc7_mip_chain(upload_bytes, decoded->width, decoded->height,
                        mip_levels, srgb);
                    break;
            }
            if (compressed) {
                format = Detail::choose_bc_format(kind, srgb);
                compressed_storage = std::move(*compressed);
                upload_bytes = span<const std::byte>{compressed_storage.data(), compressed_storage.size()};
                vector<std::byte>{}.swap(mip_chain.data);
            }
        }

        const string label_c = label.empty() ? std::string{"streamed texture"} : label.cpp_string();


        static constexpr array<RHI::QueueClass, 2> streamed_queue_classes{RHI::QueueClass::Graphics, RHI::QueueClass::Transfer};
        auto texture = impl_->renderer.create_texture(decoded->width, decoded->height, format, {}, label_c.c_str(),
                                                       span<const RHI::QueueClass>{streamed_queue_classes}, mip_levels);
        if (!texture) {
            Foundation::log_error("TextureStreamer: failed to create GPU texture for '{}': {}", source.string(), texture.error().message);
            return {};
        }
        SFT::Renderer::TextureResource *replay_resource = impl_->renderer.texture(*texture);
        if (replay_resource == nullptr) {
            impl_->renderer.destroy_texture(*texture);
            return {};
        }


        replay_resource->pixel_data.assign(upload_bytes.begin(), upload_bytes.end());
        if (auto cleared = impl_->renderer.clear_placeholder_texture(*texture, placeholder); !cleared.has_value()) {
            Foundation::log_error("TextureStreamer: failed to clear placeholder for '{}': {}", source.string(), cleared.error().message);
            impl_->renderer.destroy_texture(*texture);
            return {};
        }

        const u64 id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
        const u64 byte_count = static_cast<u64>(upload_bytes.size());
        {
            auto guard = impl_->entries.lock();
            (*guard)[id] = Impl::Entry{.state = StreamingState::Pending, .texture = *texture, .width = decoded->width,
                                       .height = decoded->height, .byte_count = byte_count};
        }
        TracyPlot("texture_streaming.queue_depth", impl_->queue_depth.fetch_add(1, std::memory_order_relaxed) + 1);
        TracyPlot("texture_streaming.in_flight_bytes",
                  impl_->in_flight_bytes.fetch_add(static_cast<i64>(byte_count), std::memory_order_relaxed) + static_cast<i64>(byte_count));

        const u32 width = decoded->width;
        const u32 height = decoded->height;


        (void)impl_->thread.run([this, id, width, height, format,
                            pixels = compressed_storage.empty() ? std::move(mip_chain.data) : std::move(compressed_storage)]() mutable {
            ZoneScopedN("TextureStreamer::upload_worker");
            RHI::RhiDevice *worker_device = impl_->renderer.rhi_device();
            SFT::Renderer::TextureHandle texture_handle{};
            {
                auto guard = impl_->entries.lock();
                if (auto it = guard->find(id); it != guard->end()) {
                    texture_handle = it->second.texture;
                    it->second.state = StreamingState::Uploading;
                }
            }
            auto mark_failed = [this, id]() {
                u64 byte_count = 0;
                {
                    auto guard = impl_->entries.lock();
                    if (auto it = guard->find(id); it != guard->end() && it->second.state != StreamingState::Failed) {
                        it->second.state = StreamingState::Failed;
                        byte_count = it->second.byte_count;
                    }
                }
                if (byte_count != 0) {
                    TracyPlot("texture_streaming.queue_depth", impl_->queue_depth.fetch_sub(1, std::memory_order_relaxed) - 1);
                    TracyPlot("texture_streaming.in_flight_bytes",
                              impl_->in_flight_bytes.fetch_sub(static_cast<i64>(byte_count), std::memory_order_relaxed) -
                                  static_cast<i64>(byte_count));
                }
            };
            if (worker_device == nullptr || !texture_handle) {
                mark_failed();
                return;
            }

            Impl::StagingChunk *ring_chunk = nullptr;
            RHI::BufferHandle staging_buffer{};
            RHI::BufferHandle owned_staging_buffer{};
            if (static_cast<u64>(pixels.size()) <= Impl::kChunkBytes) {
                auto chunk = impl_->acquire_chunk(static_cast<u64>(pixels.size()));
                if (!chunk) {
                    mark_failed();
                    return;
                }
                ring_chunk = *chunk;
                staging_buffer = ring_chunk->buffer;
            } else {
                auto dedicated = worker_device->create_buffer(RHI::BufferDesc{
                    .size = static_cast<u64>(pixels.size()),
                    .usage = RHI::BufferUsage::TransferSrc,
                    .memory = RHI::MemoryLocation::HostUpload,
                    .label = "large streamed texture staging",
                });
                if (!dedicated) {
                    mark_failed();
                    return;
                }
                owned_staging_buffer = *dedicated;
                staging_buffer = *dedicated;
            }
            if (auto written = worker_device->write_buffer(staging_buffer, 0,
                                                           span<const std::byte>{pixels.data(), pixels.size()});
                !written) {
                if (owned_staging_buffer) {
                    worker_device->destroy_buffer(owned_staging_buffer);
                }
                mark_failed();
                return;
            }

            SFT::Renderer::TextureResource *resource = impl_->renderer.texture(texture_handle);
            if (resource == nullptr) {
                if (owned_staging_buffer) {
                    worker_device->destroy_buffer(owned_staging_buffer);
                }
                mark_failed();
                return;
            }
            const RHI::QueueLane transfer_lane{RHI::QueueClass::Transfer, 0};
            auto submitted = impl_->renderer.submit_texture_upload(*resource, width, height, format,
                                                                    staging_buffer, 0, transfer_lane);
            if (!submitted) {
                if (owned_staging_buffer) {
                    worker_device->destroy_buffer(owned_staging_buffer);
                }
                mark_failed();
                return;
            }
            if (ring_chunk != nullptr) {
                ring_chunk->last_user_id = id;
            }

            {
                auto guard = impl_->entries.lock();
                if (auto it = guard->find(id); it != guard->end()) {
                    it->second.upload_fence = submitted->fence;
                    it->second.upload_command_buffer = submitted->command_buffer;
                    it->second.owned_staging_buffer = owned_staging_buffer;
                }
            }
            auto submitted_guard = impl_->submitted.lock();
            submitted_guard->push_back(id);
        });

        return StreamedTextureHandle{id};
    }

    /// Performs the state operation for `Engine` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    StreamingState TextureStreamer::state(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return StreamingState::Failed;
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? it->second.state : StreamingState::Failed;
    }

    /// Returns the texture handle associated with this `Engine`.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    SFT::Renderer::TextureHandle TextureStreamer::texture_handle(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return {};
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? it->second.texture : SFT::Renderer::TextureHandle{};
    }

    /// Performs the dimensions operation for `Engine` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    std::pair<u32, u32> TextureStreamer::dimensions(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return {0, 0};
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? std::pair{it->second.width, it->second.height} : std::pair<u32, u32>{0, 0};
    }

    /// Waits for the associated operation or synchronization primitive to complete.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void TextureStreamer::wait(StreamedTextureHandle handle) noexcept {
        ZoneScopedN("TextureStreamer::wait");
        while (state(handle) == StreamingState::Pending || state(handle) == StreamingState::Uploading) {
            pump();
        }
    }

    /// Handles the on resident callback and updates the associated platform state.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param callback Callable invoked by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextureStreamer::on_resident(StreamedTextureHandle handle, std::function<void(SFT::Renderer::TextureHandle)> callback) {
        if (!handle || !callback) {
            return;
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        if (it == guard->end()) {
            return;
        }
        if (it->second.state == StreamingState::Resident) {
            callback(it->second.texture);
            return;
        }
        if (it->second.state != StreamingState::Failed) {
            it->second.callbacks.push_back(std::move(callback));
        }
    }

    /// Pumps the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @return Returns the current pump value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextureStreamer::pump() {
        ZoneScopedN("TextureStreamer::pump");
        vector<u64> ready;
        {
            auto guard = impl_->submitted.lock();
            ready.swap(*guard);
        }
        if (ready.empty()) {
            return;
        }
        RHI::RhiDevice *device = impl_->renderer.rhi_device();
        for (const u64 id : ready) {
            SFT::Renderer::TextureHandle resolved_texture{};
            vector<std::function<void(SFT::Renderer::TextureHandle)>> callbacks;
            u64 byte_count = 0;
            bool retired = false;
            {
                auto guard = impl_->entries.lock();
                auto it = guard->find(id);
                if (it == guard->end() || it->second.state != StreamingState::Uploading || !it->second.upload_fence) {
                    continue;
                }
                Impl::Entry &entry = it->second;
                byte_count = entry.byte_count;
                if (device == nullptr) {
                    entry.state = StreamingState::Failed;
                    retired = true;
                } else {
                    auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*entry.upload_fence, 1}, true);


                    device->destroy_fence(*entry.upload_fence);
                    if (entry.upload_command_buffer) {
                        device->destroy_command_buffer(entry.upload_command_buffer);
                    }
                    if (entry.owned_staging_buffer) {
                        device->destroy_buffer(entry.owned_staging_buffer);
                    }
                    entry.upload_fence.reset();
                    entry.upload_command_buffer = {};
                    entry.owned_staging_buffer = {};
                    retired = true;
                    if (!waited || !*waited) {
                        entry.state = StreamingState::Failed;
                    } else {
                        entry.state = StreamingState::Resident;
                        resolved_texture = entry.texture;
                        callbacks.swap(entry.callbacks);
                    }
                }
            }
            if (retired) {
                TracyPlot("texture_streaming.queue_depth", impl_->queue_depth.fetch_sub(1, std::memory_order_relaxed) - 1);
                TracyPlot("texture_streaming.in_flight_bytes",
                          impl_->in_flight_bytes.fetch_sub(static_cast<i64>(byte_count), std::memory_order_relaxed) -
                              static_cast<i64>(byte_count));
            }
            for (auto &callback : callbacks) {
                callback(resolved_texture);
            }
        }
    }

} // namespace SFT::Engine

namespace SFT::Engine {

    /// Reports whether resident holds for this `Engine`.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool TextureStreamer::is_resident(StreamedTextureHandle handle) const noexcept {
        return state(handle) == StreamingState::Resident;
    }

} // namespace SFT::Engine


namespace SFT::Engine {

    /// Converts the `Engine` to `bool`.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    StreamedTextureHandle::operator bool() const noexcept { return id != 0; }

} // namespace SFT::Engine

