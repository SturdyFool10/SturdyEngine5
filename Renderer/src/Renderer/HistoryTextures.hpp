#pragma once

#include <Foundation/Foundation.hpp>

#include <string>
#include <unordered_map>

#include <Async/Async.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RenderGraph.hpp>

namespace SFT::Renderer {

    /// Textures that live from one frame to the next (temporal histories, previous-frame colour), created on
    /// demand and recreated when their size or format changes. The engine's temporal effects keep theirs here;
    /// nothing about it is private, so an application's own temporal feature can use `Renderer::history_textures()`
    /// or its own instance.
    ///
    /// Keys are chosen by the caller; `history_texture_key` combines a window, an effect id and a ping-pong slot.
    class HistoryTextureCache {
      public:
        struct Desc {
            RHI::Format format = RHI::Format::RGBA16Float;
            RHI::Extent3D extent{};
            RHI::TextureUsage usage = RHI::TextureUsage::Storage | RHI::TextureUsage::Sampled;
            const char *label = "history texture";
        };

        struct Lease {
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::Format format = RHI::Format::Undefined;
            RHI::Extent3D extent{};
            /// Frame index this texture was last written in, or `kNeverWritten`.
            u64 last_written_frame = kNeverWritten;

            /// True when the texture holds the data a frame `frame` would read as "last frame's".
            [[nodiscard]] bool written_in_previous_frame(u64 frame) const noexcept {
                return last_written_frame != kNeverWritten && last_written_frame + 1 == frame;
            }
            [[nodiscard]] bool ever_written() const noexcept { return last_written_frame != kNeverWritten; }
        };

        static constexpr u64 kNeverWritten = ~u64{0};

        /// Finds or (re)creates the texture for `key`. With `write_frame` set the texture is marked as written in that
        /// frame (the returned lease still reports the previous stamp).
        [[nodiscard]] Core::RendererExpected<Lease> acquire(RHI::RhiDevice &device, u64 key, const Desc &desc,
                                                            std::optional<u64> write_frame = std::nullopt);
        /// Destroys every texture (on `device`, if given).
        void release(RHI::RhiDevice *device) noexcept;

      private:
        struct Slot {
            const RHI::RhiDevice *device = nullptr;
            RHI::TextureHandle texture{};
            RHI::TextureViewHandle view{};
            RHI::Format format = RHI::Format::Undefined;
            RHI::Extent3D extent{};
            u64 last_written_frame = kNeverWritten;
        };
        Async::Mutex<std::unordered_map<u64, Slot>> slots_;
    };

    /// Combines a window id, an effect id (any small constant) and a ping-pong slot into a cache key.
    [[nodiscard]] constexpr u64 history_texture_key(u64 window, u32 effect, u32 slot = 0) noexcept {
        return (window << 16) ^ (static_cast<u64>(effect) << 4) ^ slot;
    }

    /// Imports a leased history texture into a frame's graph. It is kept in the `General` layout between frames so it
    /// can be both sampled and written as storage; `has_contents` says whether its current contents must be kept.
    [[nodiscard]] RenderGraphTextureHandle import_history_texture(RenderGraph &graph, const HistoryTextureCache::Lease &lease,
                                                                  bool has_contents, const char *label);

} // namespace SFT::Renderer
