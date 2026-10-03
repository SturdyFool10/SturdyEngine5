#include <Audio/AudioBuffer.hpp>
#include <Audio/Interleaved.hpp>
#include <Audio/Kernels.hpp>
#include <Audio/Channels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace SFT::Audio {

    void AudioBuffer::resize(u32 channels, u32 frames) {
        channels_ = channels;
        frames_ = frames;
        data_.assign(static_cast<usize>(channels) * frames, 0.0f);
    }

    void AudioBuffer::clear() noexcept { std::fill(data_.begin(), data_.end(), 0.0f); }

    void AudioBuffer::clear_channel(u32 channel) noexcept {
        if (channel < channels_) {
            std::ranges::fill(this->channel(channel), 0.0f);
        }
    }

    void AudioBuffer::add(const AudioBuffer &source, f32 gain, u32 frames) noexcept {
        const u32 n = std::min({frames, frames_, source.frames_});
        const u32 channels = std::min(channels_, source.channels_);
        for (const u32 c : std::views::iota(u32{0}, channels)) {
            add_channel(c, source, c, gain, n);
        }
    }

    void AudioBuffer::add_channel(u32 dst_channel, const AudioBuffer &source, u32 src_channel, f32 gain, u32 frames) noexcept {
        if (dst_channel >= channels_ || src_channel >= source.channels_) {
            return;
        }
        const u32 n = std::min({frames, frames_, source.frames_});
        const std::span<f32> out = channel(dst_channel).first(n);
        const std::span<const f32> in = source.channel(src_channel).first(n);
        if (this != &source || dst_channel != src_channel) {
            Kernels::add_gain(out, in, gain);
            return;
        }
        // Adding a channel to itself: the kernels forbid overlap, so scale in place.
        Kernels::scale(out, 1.0f + gain);
    }

    void AudioBuffer::load_interleaved(std::span<const f32> interleaved, u32 stride, u32 offset) noexcept {
        if (stride == 0 || offset >= frames_) {
            return;
        }
        const usize n = std::min<usize>(interleaved.size() / stride, frames_ - offset);
        const u32 shared = std::min(channels_, stride);
        if (shared == stride) {
            // Whole frames go straight into planes: the vectorised path.
            std::array<f32 *, max_source_channels> planes{};
            for (const u32 c : std::views::iota(u32{0}, stride)) {
                planes[c] = data(c) + offset;
            }
            Kernels::deinterleave(interleaved.data(), stride, planes.data(), n);
            return;
        }
        for (const u32 c : std::views::iota(u32{0}, shared)) {
            std::ranges::copy(channel_of(interleaved.first(n * stride), stride, c), channel(c).begin() + offset);
        }
    }

    void AudioBuffer::store_interleaved(std::span<f32> interleaved, u32 stride, u32 frames) const noexcept {
        if (stride == 0) {
            return;
        }
        const usize n = std::min<usize>({frames, frames_, interleaved.size() / stride});
        const std::span<f32> target = interleaved.first(n * stride);
        for (const u32 c : std::views::iota(u32{0}, stride)) {
            const auto lane = channel_of(target, stride, c);
            if (c < channels_) {
                std::ranges::copy(channel(c).first(n), lane.begin());
            } else {
                std::ranges::fill(lane, 0.0f);
            }
        }
    }

    void AudioBuffer::scale(f32 gain) noexcept { Kernels::scale(std::span<f32>{data_}, gain); }

    f32 AudioBuffer::peak() const noexcept { return Kernels::peak(std::span<const f32>{data_}); }

    f32 AudioBuffer::rms() const noexcept {
        if (data_.empty()) {
            return 0.0f;
        }
        const f64 sum = Kernels::sum_squares(std::span<const f32>{data_});
        return static_cast<f32>(std::sqrt(sum / static_cast<f64>(data_.size())));
    }

} // namespace SFT::Audio
