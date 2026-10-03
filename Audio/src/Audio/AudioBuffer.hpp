#pragma once

#include <Foundation/Foundation.hpp>

#include <span>
#include <vector>

namespace SFT::Audio {

    inline constexpr u32 max_channels = 32; // 22.2 = 24, 9.1.6 = 16, third-order ambisonics = 16

    /// Planar float audio: one contiguous run of `frames` samples per channel. Allocated when a graph is built and
    /// reused every block, so processing never allocates.
    class AudioBuffer {
      public:
        AudioBuffer() = default;
        AudioBuffer(u32 channels, u32 frames) { resize(channels, frames); }

        void resize(u32 channels, u32 frames);
        void clear() noexcept;
        void clear_channel(u32 channel) noexcept;

        [[nodiscard]] u32 channels() const noexcept { return channels_; }
        [[nodiscard]] u32 frames() const noexcept { return frames_; }

        [[nodiscard]] std::span<f32> channel(u32 index) noexcept { return {data_.data() + static_cast<usize>(index) * frames_, frames_}; }
        [[nodiscard]] std::span<const f32> channel(u32 index) const noexcept {
            return {data_.data() + static_cast<usize>(index) * frames_, frames_};
        }
        [[nodiscard]] f32 *data(u32 index) noexcept { return data_.data() + static_cast<usize>(index) * frames_; }
        [[nodiscard]] const f32 *data(u32 index) const noexcept { return data_.data() + static_cast<usize>(index) * frames_; }

        /// `this += source * gain` over `frames` samples of every shared channel.
        void add(const AudioBuffer &source, f32 gain = 1.0f, u32 frames = ~0u) noexcept;
        /// `this[dst_channel] += source[src_channel] * gain`.
        void add_channel(u32 dst_channel, const AudioBuffer &source, u32 src_channel, f32 gain, u32 frames = ~0u) noexcept;
        void scale(f32 gain) noexcept;
        /// Takes interleaved frames of `stride` channels: `channel(c)[offset + i] = interleaved[i * stride + c]` for the
        /// channels both sides have, as many frames as `interleaved` holds and the buffer has room for.
        void load_interleaved(std::span<const f32> interleaved, u32 stride, u32 offset = 0) noexcept;
        /// Writes the first `frames` frames as interleaved frames of `stride` channels; channels this buffer lacks are zero.
        void store_interleaved(std::span<f32> interleaved, u32 stride, u32 frames) const noexcept;
        /// Largest absolute sample over all channels.
        [[nodiscard]] f32 peak() const noexcept;
        [[nodiscard]] f32 rms() const noexcept;

      private:
        std::vector<f32> data_;
        u32 channels_ = 0;
        u32 frames_ = 0;
    };

} // namespace SFT::Audio
