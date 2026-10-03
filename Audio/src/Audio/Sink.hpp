#pragma once

#include <Audio/Channels.hpp>
#include <Audio/Mixer.hpp>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <thread>

/// Custom audio sinks: anything that wants the engine's mixed output as interleaved float frames (a network stream, a file, a
/// video encoder's audio track, a game-streaming overlay, a test). Implement `AudioSink` and let a `SinkPump` carry one
/// output of the engine to it.
namespace SFT::Audio {

    /// What a sink is about to receive.
    struct SinkFormat {
        u32 channels = 2;
        u32 sample_rate = 48000;
        ChannelLayoutInfo layout;
    };

    class AudioSink {
      public:
        virtual ~AudioSink() = default;
        /// Called once on the pump's thread before the first block; return an error to refuse the format.
        [[nodiscard]] virtual std::expected<void, UString> open(const SinkFormat &format) {
            (void)format;
            return {};
        }
        /// `interleaved.size() == frames * channels`. Runs on the pump's own thread, never the audio thread, so it may block
        /// briefly (a socket, a file write) without glitching playback.
        virtual void write(std::span<const f32> interleaved, u32 frames) = 0;
        /// Called once after the last block.
        virtual void close() {}
    };

    /// A sink made of a function.
    class CallbackSink final : public AudioSink {
      public:
        using Callback = std::function<void(std::span<const f32> interleaved, u32 frames)>;
        explicit CallbackSink(Callback callback) : callback_(std::move(callback)) {}
        void write(std::span<const f32> interleaved, u32 frames) override { callback_(interleaved, frames); }

      private:
        Callback callback_;
    };

    struct SinkPumpConfig {
        u32 block_frames = 1024;
        /// Primary output only: the pump itself drives the engine (no device sink needed), paced to real time, or as fast as
        /// the CPU allows when false (offline rendering to a sink).
        bool realtime = true;
    };

    /// Carries one output of an engine to an `AudioSink` on a thread of its own. A secondary output follows whoever pulls the
    /// primary; the primary output is driven by the pump, so do not also start a `DeviceSink` on the same engine for it.
    class SinkPump {
      public:
        [[nodiscard]] static std::expected<std::unique_ptr<SinkPump>, UString> start(AudioEngine &engine, OutputId output, std::shared_ptr<AudioSink> sink,
                                                                                         const SinkPumpConfig &config = {});
        ~SinkPump();
        SinkPump(const SinkPump &) = delete;
        SinkPump &operator=(const SinkPump &) = delete;

        [[nodiscard]] u64 frames_delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }
        [[nodiscard]] const SinkFormat &format() const noexcept { return format_; }

      private:
        SinkPump() = default;
        void run();

        AudioEngine *engine_ = nullptr;
        OutputId output_ = 0;
        std::shared_ptr<AudioSink> sink_;
        SinkPumpConfig config_;
        SinkFormat format_;
        std::atomic<bool> stop_{false};
        std::atomic<u64> delivered_{0};
        std::thread worker_;
    };

} // namespace SFT::Audio
