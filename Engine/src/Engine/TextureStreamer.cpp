#include "TextureStreamer.hpp"
#include "ImageDecode.hpp"
#include "TextureCompression.hpp"
#include "TextureMipChain.hpp"

#include <Async/src/Affinity.hpp>
#include <Async/src/Mutex.hpp>
#include <Renderer/Renderer.hpp>

// Per-platform fast-file-read backends, both optimizations over the std::ifstream fallback below
// -- see their own doc comments for what each does and doesn't touch. Every other platform (MacOS,
// FreeBSD, Web) has no fast-path backend at all and always takes the std::ifstream tier, which is
// this function's general compatibility fallback, not just Windows/Linux's own error path.
#if defined(_WIN32)
    #include <Core/src/Core/Vulkan/DirectStorage/DirectStorageBackend.hpp>
#elif defined(__linux__)
    #include <Core/src/Core/IoUring/IoUringBackend.hpp>
#endif

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

        // Deliberately duplicated (not shared) from AssetManager.cpp's own anonymous-namespace
        // read_binary_file: both are small, self-contained, and pulling this into a shared header
        // purely to avoid ~15 duplicated lines isn't worth the coupling right now.
        [[nodiscard]] AssetExpected<vector<std::byte>> read_binary_file_streamed(const std::filesystem::path &source) {
            // Try the platform fast-read backend first -- DirectStorage on Windows, io_uring on
            // Linux (see their own doc comments for exactly what each does and does not touch); no
            // backend exists for any other platform. Call the read entry point directly: it performs
            // its own one-time availability check, avoiding a redundant lock/acquire cycle for every
            // streamed texture. Any failure -- unavailable, or a per-request error -- falls straight
            // through to the std::ifstream tier below rather than propagating: every backend here is
            // strictly an optimization, never the only way to read a texture, and std::ifstream is
            // the one path guaranteed to work everywhere (including a platform/backend combination
            // that regresses or a sandbox that blocks the fast-path syscalls/APIs outright).
#if defined(_WIN32)
            if (auto bytes = Core::read_file_direct_storage(source)) {
                return std::move(*bytes);
            }
#elif defined(__linux__)
            if (auto bytes = Core::read_file_io_uring(source)) {
                return std::move(*bytes);
            }
#endif
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
        // Fence/command-buffer ownership contract: `upload_fence`/`upload_command_buffer` below are
        // owned exclusively by pump() (main thread) once set -- it is the only code in this class
        // that ever destroys them, always from inside `entries`'s lock, always immediately after
        // transitioning `state` away from Uploading. Every other reader (acquire_chunk, on the
        // background thread) treats `state == Uploading` as "the fence handle is still live and safe
        // to wait on" and anything else as "already retired, do not touch the handle." This single-
        // owner rule is what a naive per-chunk fence copy (an earlier version of this file had one)
        // got wrong -- two independent destroyers of the same handle is a use-after-free waiting to
        // happen the moment ring rotation and pump() timing interleave badly.
        struct Entry {
            StreamingState state = StreamingState::Pending;
            SFT::Renderer::TextureHandle texture{};
            u32 width = 0;
            u32 height = 0;
            u64 byte_count = 0; // for the in_flight_bytes Tracy plot -- see request_texture_load/pump
            vector<std::function<void(SFT::Renderer::TextureHandle)>> callbacks;
            std::optional<RHI::FenceHandle> upload_fence;
            RHI::CommandBufferHandle upload_command_buffer{};
            // Non-null only when the upload exceeded the reusable ring-chunk size. Kept alive until
            // pump() observes the upload fence, then destroyed alongside the command buffer.
            RHI::BufferHandle owned_staging_buffer{};
        };

        // One reusable HostUpload staging buffer. `last_user_id` records which request's bytes are
        // currently sitting in it; acquire_chunk consults `entries[last_user_id]` (not a local copy)
        // to decide whether it's safe to overwrite -- see the Entry doc comment above for why.
        struct StagingChunk {
            RHI::BufferHandle buffer{};
            u64 last_user_id = 0; // 0 == never used
        };

        static constexpr usize kRingSize = 4;
        static constexpr u64 kChunkBytes = 16ull * 1024 * 1024;

        explicit Impl(SFT::Renderer::Renderer &renderer_ref) : renderer(renderer_ref), thread("TextureStreamer") {}

        // Returns a chunk sized at least `bytes`, reused from the ring when `bytes <= kChunkBytes`
        // (waiting out the chunk's previous occupant first, if it hasn't been retired by pump() yet
        // -- backpressure by design: the background thread naturally throttles to how fast the GPU
        // actually drains uploads and pump() actually runs). Larger textures are rejected here; the
        // caller falls back to a dedicated one-off buffer it owns/destroys itself.
        // Background thread only.
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
                // If the entry is no longer Uploading, pump() already waited on and destroyed this
                // fence -- nothing to do, the buffer is already safe to overwrite.
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
        // IDs whose background upload has been submitted (not yet waited on) -- drained by pump().
        Async::Mutex<vector<u64>> submitted;

        // Tracy plots (see request_texture_load/pump for the increment/decrement sites): queue_depth
        // counts requests not yet Resident/Failed, in_flight_bytes their total pixel-data size.
        std::atomic<i64> queue_depth{0};
        std::atomic<i64> in_flight_bytes{0};

        // Background thread only -- no lock needed (request handling is entirely serial on this one
        // Async::DedicatedThread).
        array<StagingChunk, kRingSize> ring{};
        usize ring_cursor = 0;
        bool ring_initialized = false;
    };

    TextureStreamer::TextureStreamer(SFT::Renderer::Renderer &renderer) : impl_(std::make_unique<Impl>(renderer)) {}

    TextureStreamer::~TextureStreamer() = default;

    StreamedTextureHandle TextureStreamer::request_texture_load(
        std::filesystem::path source, TextureColorSpace color_space, RHI::ClearColor placeholder, UString label) {
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
            decoded->pixels, decoded->width, decoded->height, srgb);
        if (!generated_mips) {
            Foundation::log_error("TextureStreamer: failed to generate mip levels for '{}'.", source.string());
            return {};
        }
        Detail::TextureMipChain mip_chain = std::move(*generated_mips);
        vector<std::byte>{}.swap(decoded->pixels);
        const u32 mip_levels = mip_chain.mip_levels;
        span<const std::byte> upload_bytes{mip_chain.data.data(), mip_chain.data.size()};

        // Preserve the BC7 VRAM win by compressing every mip level rather than only the base image.
        vector<std::byte> compressed_storage;
        RHI::RhiDevice *device = impl_->renderer.rhi_device();
        if (decoded->width >= 4 && decoded->height >= 4 && device != nullptr &&
            device->limits().supports_bc_texture_compression) {
            if (auto compressed = Detail::compress_bc7_mip_chain(
                    upload_bytes, decoded->width, decoded->height, mip_levels, srgb)) {
                format = srgb ? RHI::Format::BC7UnormSrgb : RHI::Format::BC7Unorm;
                compressed_storage = std::move(*compressed);
                upload_bytes = span<const std::byte>{compressed_storage.data(), compressed_storage.size()};
                vector<std::byte>{}.swap(mip_chain.data);
            }
        }

        const string label_c = label.empty() ? std::string{"streamed texture"} : label.cpp_string();
        // Written by the background thread's later upload submission on the Transfer queue, sampled
        // by Graphics -- VK_SHARING_MODE_CONCURRENT across both (see TextureDesc::
        // concurrent_queue_classes's own doc comment for why CONCURRENT was chosen over an explicit
        // queue-family-ownership-transfer barrier pair for this streaming path).
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
        // The allocation intentionally started empty to avoid a synchronous upload, but renderer-owned
        // textures still need authoritative CPU data for device recovery. Publish it on this request
        // thread before launching the worker rather than mutating Renderer::textures_ asynchronously.
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
        // Fire-and-forget: this request's completion is observed via `entries`/`submitted` (polled by
        // state()/pump()), not via the TaskHandle -- discarded deliberately, not an oversight.
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

    StreamingState TextureStreamer::state(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return StreamingState::Failed;
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? it->second.state : StreamingState::Failed;
    }

    SFT::Renderer::TextureHandle TextureStreamer::texture_handle(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return {};
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? it->second.texture : SFT::Renderer::TextureHandle{};
    }

    std::pair<u32, u32> TextureStreamer::dimensions(StreamedTextureHandle handle) const noexcept {
        if (!handle) {
            return {0, 0};
        }
        auto guard = impl_->entries.lock();
        auto it = guard->find(handle.id);
        return it != guard->end() ? std::pair{it->second.width, it->second.height} : std::pair<u32, u32>{0, 0};
    }

    void TextureStreamer::wait(StreamedTextureHandle handle) noexcept {
        ZoneScopedN("TextureStreamer::wait");
        while (state(handle) == StreamingState::Pending || state(handle) == StreamingState::Uploading) {
            pump();
        }
    }

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
                    // Sole owner of these handles (see Entry's own doc comment) -- destroy them here,
                    // under this same lock, in the same step that leaves `Uploading`, so any concurrent
                    // acquire_chunk() reader on the background thread that observes state != Uploading
                    // is guaranteed the handles are already gone and must not touch them.
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
