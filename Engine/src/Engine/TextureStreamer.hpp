#pragma once

#include "AssetManager.hpp"

#include <Renderer/Handles.hpp>
#include <RHI/RHI.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

namespace SFT::Renderer {
    class Renderer;
}

namespace SFT::Engine {

    enum class StreamingState : u8 { Pending, Uploading, Resident, Failed };

    /// Opaque handle to one in-flight or completed streamed texture load. Unlike Asset, this is not
    /// registered with AssetManager -- TextureStreamer is a lower-level building block AssetManager's
    /// own streamed entry point (a future addition) would sit on top of, and is also usable directly by
    /// callers that want streamed textures without going through the Asset system at all.
    struct StreamedTextureHandle {
        u64 id = 0;
        [[nodiscard]] explicit operator bool() const noexcept;
    };

    /// Asynchronous GPU texture streaming: request_texture_load() mints a real, immediately-bindable
    /// RHI texture and returns without blocking on the disk-to-GPU round trip, unlike
    /// Renderer::create_texture()/AssetManager::load_texture() which block the calling thread until the
    /// full upload's fence has signaled. See RendererModule.hpp's clear_placeholder_texture/
    /// submit_texture_upload doc comments for the RHI-level mechanism this builds on: the texture's
    /// image/view/sampler are created (and cleared to `placeholder`) immediately and synchronously, and
    /// only the pixel upload -- staging-buffer write, GPU copy, submit, fence wait -- is deferred to a
    /// dedicated background thread. The returned handle's underlying RHI::TextureHandle never changes
    /// and is safe to bind into descriptors/materials the instant request_texture_load() returns.
    ///
    /// Scope note (v1): the file read and CPU decode/BC7-compress steps stay on the CALLING thread,
    /// synchronously, inside request_texture_load() -- only the disk-to-GPU staging+upload+fence-wait
    /// moves to the background thread. This is a deliberate scoping decision, not an oversight: that
    /// GPU round trip (whose latency depends on queue depth/driver scheduling, not just local CPU work)
    /// was the actual unbounded-latency cost in the original synchronous path
    /// (Renderer::create_texture -> upload_texture_rgba -> wait_fences(wait_forever)), while file
    /// read/decode is fast, deterministic, CPU-only work. A future revision can move decode to the
    /// background thread too (it would need a cheap header-only dimension peek before minting the
    /// texture, since width/height must be known before RhiDevice::create_texture() can be called).
    class TextureStreamer {
      public:
        explicit TextureStreamer(SFT::Renderer::Renderer &renderer);
        ~TextureStreamer();

        TextureStreamer(const TextureStreamer &) = delete;
        TextureStreamer &operator=(const TextureStreamer &) = delete;
        TextureStreamer(TextureStreamer &&) = delete;
        TextureStreamer &operator=(TextureStreamer &&) = delete;

        /// Reads + decodes `source` synchronously (see class doc comment for why), mints the texture,
        /// clears it to `placeholder`, and enqueues the background upload. Returns an invalid handle
        /// (operator bool() false) only if the synchronous read/decode/mint/clear steps fail outright --
        /// once a valid handle is returned, its underlying RHI::TextureHandle is guaranteed bindable.
        [[nodiscard]] StreamedTextureHandle request_texture_load(
            std::filesystem::path source, TextureColorSpace color_space = TextureColorSpace::Srgb,
            TextureKind kind = TextureKind::ColorAlpha,
            RHI::ClearColor placeholder = RHI::ClearColor{1.0f, 0.0f, 1.0f, 1.0f}, UString label = {});

        [[nodiscard]] StreamingState state(StreamedTextureHandle handle) const noexcept;
        [[nodiscard]] bool is_resident(StreamedTextureHandle handle) const noexcept;
        /// The RHI-backing texture -- valid (bindable) immediately after request_texture_load()
        /// returns a valid handle, regardless of streaming state; see the class doc comment.
        [[nodiscard]] SFT::Renderer::TextureHandle texture_handle(StreamedTextureHandle handle) const noexcept;
        /// Pixel dimensions decoded during request_texture_load() -- {0, 0} for an invalid handle.
        /// Known synchronously (unlike streaming state), since decode happens before that call returns.
        [[nodiscard]] std::pair<u32, u32> dimensions(StreamedTextureHandle handle) const noexcept;

        /// Blocks the calling thread until `handle` reaches Resident or Failed. For callers that want
        /// today's synchronous-load behavior on top of the streaming path (e.g. a temporary migration
        /// shim); ordinary streaming callers should prefer polling state()/is_resident() or on_resident().
        void wait(StreamedTextureHandle handle) noexcept;

        /// Registers a callback fired from pump() (i.e. on whichever thread calls pump(), expected to
        /// be the main/render thread) once `handle` reaches Resident. Fired immediately, synchronously,
        /// if `handle` is already Resident at registration time. Never fired for a Failed handle.
        void on_resident(StreamedTextureHandle handle, std::function<void(SFT::Renderer::TextureHandle)> callback);

        /// Call once per frame from the main/render thread: waits (briefly -- these fences are expected
        /// already signaled by the time pump() gets to them in steady state) on any uploads the
        /// background thread has finished submitting, retires their command buffers, marks the
        /// corresponding entries Resident, and fires their on_resident() callbacks.
        void pump();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace SFT::Engine
